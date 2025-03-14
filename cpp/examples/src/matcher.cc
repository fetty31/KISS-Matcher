/**
 * Copyright 2020, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Jingnan Shi, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 */

#include "quatro/matcher.h"

#include <random>

#include <Eigen/Core>
#include <flann/flann.hpp>
#include <pcl/point_cloud.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include "teaser/geometry.h"

namespace teaser {

std::vector<std::pair<int, int>> Matcher::calculateCorrespondences(
    teaser::PointCloud& source_points,
    teaser::PointCloud& target_points,
    teaser::FPFHCloud& source_features,
    teaser::FPFHCloud& target_features,
    bool use_absolute_scale,
    bool use_crosscheck,
    bool use_tuple_test,
    float tuple_scale,
    bool use_optimized_matching) {
  Feature cloud_features;
  pointcloud_.push_back(source_points);
  pointcloud_.push_back(target_points);

  // It compute the global_scale_ required to set correctly the search radius
  if (!use_optimized_matching) {
    normalizePoints(use_absolute_scale);
  }

  for (auto& f : source_features) {
    Eigen::VectorXf fpfh(33);
    for (int i = 0; i < 33; i++) fpfh(i) = f.histogram[i];
    cloud_features.push_back(fpfh);
  }
  features_.push_back(cloud_features);

  cloud_features.clear();
  for (auto& f : target_features) {
    Eigen::VectorXf fpfh(33);
    for (int i = 0; i < 33; i++) fpfh(i) = f.histogram[i];
    cloud_features.push_back(fpfh);
  }
  features_.push_back(cloud_features);

  if (use_optimized_matching) {
    std::cout << "\033[1;32mUse optimized matching!\033[0m\n";
    optimizedMatching(thr_dist_, num_max_corres_, tuple_scale);
  } else {
    advancedMatching(use_crosscheck, use_tuple_test, tuple_scale);
  }
  return corres_;
}

void Matcher::normalizePoints(bool use_absolute_scale) {
  int num     = 2;
  float scale = 0;

  means_.clear();

  for (int i = 0; i < num; ++i) {
    float max_scale = 0;

    // compute mean
    Eigen::Vector3f mean;
    mean.setZero();

    int npti = pointcloud_[i].size();
    for (int ii = 0; ii < npti; ++ii) {
      Eigen::Vector3f p(pointcloud_[i][ii].x, pointcloud_[i][ii].y, pointcloud_[i][ii].z);
      mean = mean + p;
    }
    mean = mean / npti;
    means_.push_back(mean);

    for (int ii = 0; ii < npti; ++ii) {
      pointcloud_[i][ii].x -= mean(0);
      pointcloud_[i][ii].y -= mean(1);
      pointcloud_[i][ii].z -= mean(2);
    }

    // compute scale
    for (int ii = 0; ii < npti; ++ii) {
      Eigen::Vector3f p(pointcloud_[i][ii].x, pointcloud_[i][ii].y, pointcloud_[i][ii].z);
      float temp = p.norm();  // because we extract mean in the previous stage.
      if (temp > max_scale) {
        max_scale = temp;
      }
    }

    if (max_scale > scale) {
      scale = max_scale;
    }
  }

  // mean of the scale variation
  if (use_absolute_scale) {
    global_scale_ = 1.0f;
  } else {
    global_scale_ = scale;  // second choice: we keep the maximum scale.
  }

  if (global_scale_ != 1.0f) {
    for (int i = 0; i < num; ++i) {
      int npti = pointcloud_[i].size();
      for (int ii = 0; ii < npti; ++ii) {
        pointcloud_[i][ii].x /= global_scale_;
        pointcloud_[i][ii].y /= global_scale_;
        pointcloud_[i][ii].z /= global_scale_;
      }
    }
  }
}

void Matcher::advancedMatching(bool use_crosscheck, bool use_tuple_test, float tuple_scale) {
  int fi = 0;  // source idx
  int fj = 1;  // destination idx

  bool swapped = false;

  if (pointcloud_[fj].size() > pointcloud_[fi].size()) {
    int temp = fi;
    fi       = fj;
    fj       = temp;
    swapped  = true;
  }

  int nPti = pointcloud_[fi].size();
  int nPtj = pointcloud_[fj].size();

  ///////////////////////////
  /// Build FLANNTREE
  ///////////////////////////
  KDTree feature_tree_i(flann::KDTreeSingleIndexParams(15));
  buildKDTree(features_[fi], &feature_tree_i);

  KDTree feature_tree_j(flann::KDTreeSingleIndexParams(15));
  buildKDTree(features_[fj], &feature_tree_j);

  std::vector<int> corres_K(1, 0);
  std::vector<float> dis(1, 0.0);

  std::vector<std::pair<int, int>> corres;
  std::vector<std::pair<int, int>> corres_cross;
  std::vector<std::pair<int, int>> corres_ij;
  std::vector<std::pair<int, int>> corres_ji;

  ///////////////////////////
  /// INITIAL MATCHING
  ///////////////////////////
  std::vector<int> i_to_j(nPti, -1);
  for (int j = 0; j < nPtj; j++) {
    searchKDTree(&feature_tree_i, features_[fj][j], corres_K, dis, 1);
    int i = corres_K[0];
    if (i_to_j[i] == -1) {
      searchKDTree(&feature_tree_j, features_[fi][i], corres_K, dis, 1);
      int ij    = corres_K[0];
      i_to_j[i] = ij;
    }
    corres_ji.push_back(std::pair<int, int>(i, j));
  }

  for (int i = 0; i < nPti; i++) {
    if (i_to_j[i] != -1) corres_ij.push_back(std::pair<int, int>(i, i_to_j[i]));
  }

  int ncorres_ij = corres_ij.size();
  int ncorres_ji = corres_ji.size();

  // corres = corres_ij + corres_ji;
  for (int i = 0; i < ncorres_ij; ++i)
    corres.push_back(std::pair<int, int>(corres_ij[i].first, corres_ij[i].second));
  for (int j = 0; j < ncorres_ji; ++j)
    corres.push_back(std::pair<int, int>(corres_ji[j].first, corres_ji[j].second));

  ///////////////////////////
  /// CROSS CHECK
  /// input : corres_ij, corres_ji
  /// output : corres
  ///////////////////////////
  if (use_crosscheck) {
    std::cout << "CROSS CHECK" << std::endl;
    // build data structure for cross check
    corres.clear();
    corres_cross.clear();
    std::vector<std::vector<int>> Mi(nPti);
    std::vector<std::vector<int>> Mj(nPtj);

    int ci, cj;
    for (int i = 0; i < ncorres_ij; ++i) {
      ci = corres_ij[i].first;
      cj = corres_ij[i].second;
      Mi[ci].push_back(cj);
    }
    for (int j = 0; j < ncorres_ji; ++j) {
      ci = corres_ji[j].first;
      cj = corres_ji[j].second;
      Mj[cj].push_back(ci);
    }

    // cross check
    for (int i = 0; i < nPti; ++i) {
      for (int ii = 0; ii < Mi[i].size(); ++ii) {
        int j = Mi[i][ii];
        for (int jj = 0; jj < Mj[j].size(); ++jj) {
          if (Mj[j][jj] == i) {
            corres.push_back(std::pair<int, int>(i, j));
            corres_cross.push_back(std::pair<int, int>(i, j));
          }
        }
      }
    }
  } else {
    std::cout << "Skipping Cross Check." << std::endl;
  }

  ///////////////////////////
  /// TUPLE CONSTRAINT
  /// input : corres
  /// output : corres
  ///////////////////////////
  if (use_tuple_test && tuple_scale != 0) {
    std::cout << "TUPLE CONSTRAINT" << std::endl;
    srand(time(NULL));
    int rand0, rand1, rand2;
    int idi0, idi1, idi2;
    int idj0, idj1, idj2;
    float scale         = tuple_scale;
    int ncorr           = corres.size();
    int number_of_trial = ncorr * 100;
    std::vector<std::pair<int, int>> corres_tuple;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distribution(0, ncorr - 1);
    for (int i = 0; i < number_of_trial; i++) {
      rand0 = distribution(gen) % ncorr;
      rand1 = distribution(gen) % ncorr;
      rand2 = distribution(gen) % ncorr;

      idi0 = corres[rand0].first;
      idj0 = corres[rand0].second;
      idi1 = corres[rand1].first;
      idj1 = corres[rand1].second;
      idi2 = corres[rand2].first;
      idj2 = corres[rand2].second;

      // collect 3 points from i-th fragment
      Eigen::Vector3f pti0 = {
          pointcloud_[fi][idi0].x, pointcloud_[fi][idi0].y, pointcloud_[fi][idi0].z};
      Eigen::Vector3f pti1 = {
          pointcloud_[fi][idi1].x, pointcloud_[fi][idi1].y, pointcloud_[fi][idi1].z};
      Eigen::Vector3f pti2 = {
          pointcloud_[fi][idi2].x, pointcloud_[fi][idi2].y, pointcloud_[fi][idi2].z};

      float li0 = (pti0 - pti1).norm();
      float li1 = (pti1 - pti2).norm();
      float li2 = (pti2 - pti0).norm();

      // collect 3 points from j-th fragment
      Eigen::Vector3f ptj0 = {
          pointcloud_[fj][idj0].x, pointcloud_[fj][idj0].y, pointcloud_[fj][idj0].z};
      Eigen::Vector3f ptj1 = {
          pointcloud_[fj][idj1].x, pointcloud_[fj][idj1].y, pointcloud_[fj][idj1].z};
      Eigen::Vector3f ptj2 = {
          pointcloud_[fj][idj2].x, pointcloud_[fj][idj2].y, pointcloud_[fj][idj2].z};

      float lj0 = (ptj0 - ptj1).norm();
      float lj1 = (ptj1 - ptj2).norm();
      float lj2 = (ptj2 - ptj0).norm();

      if ((li0 * scale < lj0) && (lj0 < li0 / scale) && (li1 * scale < lj1) &&
          (lj1 < li1 / scale) && (li2 * scale < lj2) && (lj2 < li2 / scale)) {
        corres_tuple.push_back(std::pair<int, int>(idi0, idj0));
        corres_tuple.push_back(std::pair<int, int>(idi1, idj1));
        corres_tuple.push_back(std::pair<int, int>(idi2, idj2));
      }
    }
    corres.clear();

    for (size_t i = 0; i < corres_tuple.size(); ++i)
      corres.push_back(std::pair<int, int>(corres_tuple[i].first, corres_tuple[i].second));
  } else {
    std::cout << "Skipping Tuple Constraint." << std::endl;
  }

  if (swapped) {
    std::vector<std::pair<int, int>> temp;
    for (size_t i = 0; i < corres.size(); i++)
      temp.push_back(std::pair<int, int>(corres[i].second, corres[i].first));
    corres.clear();
    corres = temp;
  }
  corres_ = corres;

  ///////////////////////////
  /// ERASE DUPLICATES
  /// input : corres_
  /// output : corres_
  ///////////////////////////
  std::sort(corres_.begin(), corres_.end());
  corres_.erase(std::unique(corres_.begin(), corres_.end()), corres_.end());
}

void Matcher::optimizedMatching(float thr_dist, int num_max_corres, float tuple_scale) {
  int fi = 0;  // source idx
  int fj = 1;  // destination idx

  bool swapped = false;

  if (pointcloud_[fj].size() > pointcloud_[fi].size()) {
    int temp = fi;
    fi       = fj;
    fj       = temp;
    swapped  = true;
  }

  int nPti = pointcloud_[fi].size();
  int nPtj = pointcloud_[fj].size();

  ///////////////////////////
  /// Build FLANNTREE
  ///////////////////////////
  std::chrono::steady_clock::time_point begin_build = std::chrono::steady_clock::now();
  KDTree feature_tree_i(flann::KDTreeSingleIndexParams(15));
  buildKDTree(features_[fi], &feature_tree_i);

  KDTree feature_tree_j(flann::KDTreeSingleIndexParams(15));
  buildKDTree(features_[fj], &feature_tree_j);
  std::chrono::steady_clock::time_point end_build = std::chrono::steady_clock::now();

  std::vector<int> corres_K;
  std::vector<float> dis;

  ///////////////////////////
  /// INITIAL MATCHING
  ///////////////////////////
  searchKDTreeAll(&feature_tree_i, features_[fj], corres_K, dis, 1);
  std::chrono::steady_clock::time_point end_search = std::chrono::steady_clock::now();

  std::vector<int> i_to_j(nPti, -1);
  std::vector<std::pair<int, int>> empty_vector;
  empty_vector.reserve(corres_K.size());
  std::vector<int> corres_K_for_i(1, 0);
  std::vector<float> dis_for_i(1, 0.0);

  std::chrono::steady_clock::time_point begin_corr = std::chrono::steady_clock::now();
  auto corres                                      = tbb::parallel_reduce(
      // Range
      tbb::blocked_range<size_t>(0, corres_K.size()),
      // Identity
      empty_vector,
      // 1st lambda: Parallel computation
      [&](const tbb::blocked_range<size_t>& r,
          std::vector<std::pair<int, int>> local_corres) -> std::vector<std::pair<int, int>> {
        local_corres.reserve(r.size());
        for (size_t j = r.begin(); j != r.end(); ++j) {
          if (dis[j] > thr_dist * thr_dist) {
            continue;
          }

          const int& i = corres_K[j];
          if (i_to_j[i] == -1) {
            searchKDTree(&feature_tree_j, features_[fi][i], corres_K_for_i, dis_for_i, 1);
            i_to_j[i] = corres_K_for_i[0];
            if (corres_K_for_i[0] == j) {
              local_corres.emplace_back(i, j);
            }
          }
        }
        return local_corres;
      },
      // 2nd lambda: Parallel reduction
      [](std::vector<std::pair<int, int>> a,
         const std::vector<std::pair<int, int>>& b) -> std::vector<std::pair<int, int>> {
        a.insert(a.end(),  //
                 std::make_move_iterator(b.begin()),
                 std::make_move_iterator(b.end()));
        return a;
      });
  std::chrono::steady_clock::time_point end_corr = std::chrono::steady_clock::now();

  ///////////////////////////
  /// TUPLE TEST
  ///////////////////////////
  if (tuple_scale != 0) {
    std::cout << "TUPLE CONSTRAINT" << std::endl;
    srand(time(NULL));
    int rand0, rand1, rand2;
    int idi0, idi1, idi2;
    int idj0, idj1, idj2;
    float scale         = tuple_scale;
    int ncorr           = corres.size();
    int number_of_trial = ncorr * 100;

    std::vector<bool> is_already_included(ncorr, false);
    corres_.clear();
    corres_.reserve(num_max_corres);

    auto addUniqueCorrespondence = [&](const int randIndex, const int id1, const int id2) {
      if (!is_already_included[randIndex]) {
        corres_.emplace_back(id1, id2);
        is_already_included[randIndex] = true;
      }
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distribution(0, ncorr - 1);
    for (int i = 0; i < number_of_trial; i++) {
      rand0 = distribution(gen) % ncorr;
      rand1 = distribution(gen) % ncorr;

      idi0 = corres[rand0].first;
      idj0 = corres[rand0].second;
      idi1 = corres[rand1].first;
      idj1 = corres[rand1].second;

      // The order has been changed to reduce the redundant computation
      Eigen::Vector3f pti0 = {
          pointcloud_[fi][idi0].x, pointcloud_[fi][idi0].y, pointcloud_[fi][idi0].z};
      Eigen::Vector3f pti1 = {
          pointcloud_[fi][idi1].x, pointcloud_[fi][idi1].y, pointcloud_[fi][idi1].z};

      Eigen::Vector3f ptj0 = {
          pointcloud_[fj][idj0].x, pointcloud_[fj][idj0].y, pointcloud_[fj][idj0].z};
      Eigen::Vector3f ptj1 = {
          pointcloud_[fj][idj1].x, pointcloud_[fj][idj1].y, pointcloud_[fj][idj1].z};

      float li0 = (pti0 - pti1).norm();
      float lj0 = (ptj0 - ptj1).norm();

      if ((li0 * scale > lj0) || (lj0 > li0 / scale)) {
        continue;
      }

      rand2 = distribution(gen) % ncorr;
      idi2  = corres[rand2].first;
      idj2  = corres[rand2].second;

      Eigen::Vector3f pti2 = {
          pointcloud_[fi][idi2].x, pointcloud_[fi][idi2].y, pointcloud_[fi][idi2].z};
      Eigen::Vector3f ptj2 = {
          pointcloud_[fj][idj2].x, pointcloud_[fj][idj2].y, pointcloud_[fj][idj2].z};

      float li1 = (pti1 - pti2).norm();
      float li2 = (pti2 - pti0).norm();

      float lj1 = (ptj1 - ptj2).norm();
      float lj2 = (ptj2 - ptj0).norm();

      if ((li1 * scale < lj1) && (lj1 < li1 / scale) && (li2 * scale < lj2) &&
          (lj2 < li2 / scale)) {
        if (swapped) {
          addUniqueCorrespondence(rand0, idj0, idi0);
          addUniqueCorrespondence(rand1, idj1, idi1);
          addUniqueCorrespondence(rand2, idj2, idi2);
        } else {
          addUniqueCorrespondence(rand0, idi0, idj0);
          addUniqueCorrespondence(rand1, idi1, idj1);
          addUniqueCorrespondence(rand2, idi2, idj2);
        }
      }
      if (corres_.size() > num_max_corres) {
        break;
      }
    }
  } else {
    std::cout << "Skipping Tuple Constraint." << std::endl;
  }

  std::chrono::steady_clock::time_point end_tuple_test = std::chrono::steady_clock::now();
  const int width                                      = 25;
  std::cout
      << std::setw(width) << "[Build KdTree]: "
      << std::chrono::duration_cast<std::chrono::microseconds>(end_build - begin_build).count() /
             1000000.0
      << " sec" << std::endl;
  std::cout
      << std::setw(width) << "[Search using FLANN]: "
      << std::chrono::duration_cast<std::chrono::microseconds>(end_search - end_build).count() /
             1000000.0
      << " sec" << std::endl;
  std::cout
      << std::setw(width) << "[Cross checking]: "
      << std::chrono::duration_cast<std::chrono::microseconds>(end_corr - end_search).count() /
             1000000.0
      << " sec" << std::endl;
  std::cout
      << std::setw(width) << "[Tuple test]: "
      << std::chrono::duration_cast<std::chrono::microseconds>(end_tuple_test - end_corr).count() /
             1000000.0
      << " sec" << std::endl;
}

template <typename T>
void Matcher::buildKDTree(const std::vector<T>& data, Matcher::KDTree* tree) {
  int rows, dim;
  rows = static_cast<int>(data.size());
  dim  = static_cast<int>(data[0].size());
  std::vector<float> dataset(rows * dim);
  flann::Matrix<float> dataset_mat(&dataset[0], rows, dim);
  for (int i = 0; i < rows; i++)
    for (int j = 0; j < dim; j++) dataset[i * dim + j] = data[i][j];
  KDTree temp_tree(dataset_mat, flann::KDTreeSingleIndexParams(15));
  temp_tree.buildIndex();
  *tree = temp_tree;
}

template <typename T>
void Matcher::searchKDTree(Matcher::KDTree* tree,
                           const T& input,
                           std::vector<int>& indices,
                           std::vector<float>& dists,
                           int nn) {
  int rows_t = 1;
  int dim    = input.size();

  std::vector<float> query;
  query.resize(rows_t * dim);
  for (int i = 0; i < dim; i++) query[i] = input(i);
  flann::Matrix<float> query_mat(&query[0], rows_t, dim);

  flann::Matrix<int> indices_mat(&indices[0], rows_t, nn);
  flann::Matrix<float> dists_mat(&dists[0], rows_t, nn);
  tree->knnSearch(query_mat, indices_mat, dists_mat, nn, flann::SearchParams(128));
}

template <typename T>
void Matcher::buildKDTreeWithTBB(const std::vector<T>& data, Matcher::KDTree* tree) {
  // Note that it is not much faster than `buildKDTrees`, which is without TBB
  // I guess the reason is that the data is too small and the overhead of TBB is a bit larger than
  // the speedup.
  int rows, dim;
  rows = static_cast<int>(data.size());
  dim  = static_cast<int>(data[0].size());
  std::vector<float> dataset(rows * dim);
  flann::Matrix<float> dataset_mat(&dataset[0], rows, dim);
  tbb::parallel_for(0, rows, 1, [&](int i) {
    for (int j = 0; j < dim; j++) {
      dataset[i * dim + j] = data[i][j];
    }
  });
  KDTree temp_tree(dataset_mat, flann::KDTreeSingleIndexParams(15));
  temp_tree.buildIndex();
  *tree = temp_tree;
}

template <typename T>
void Matcher::searchKDTreeAll(Matcher::KDTree* tree,
                              const std::vector<T>& inputs,
                              std::vector<int>& indices,
                              std::vector<float>& dists,
                              int nn) {
  int dim = inputs[0].size();

  std::vector<float> query(inputs.size() * dim);
  for (size_t i = 0; i < inputs.size(); ++i) {
    for (int j = 0; j < dim; ++j) {
      query[i * dim + j] = inputs[i](j);
    }
  }
  flann::Matrix<float> query_mat(&query[0], inputs.size(), dim);

  indices.resize(inputs.size() * nn);
  dists.resize(inputs.size() * nn);
  flann::Matrix<int> indices_mat(&indices[0], inputs.size(), nn);
  flann::Matrix<float> dists_mat(&dists[0], inputs.size(), nn);

  auto flann_params  = flann::SearchParams(128);
  flann_params.cores = 12;
  tree->knnSearch(query_mat, indices_mat, dists_mat, nn, flann_params);
}
}  // namespace teaser
