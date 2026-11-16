// The active set (`active_floor`): which cells an update visits. Blocks near the mass are updated cell by
// cell; every other block is background, one mass for all of its cells (docs/design.md).

#include <algorithm>
#include <cmath>
#include <numeric>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

double GridFilter::background_mass() const {
  double sum = 0.0;
  for (const double m : bg_mass_) sum += m;
  return sum;
}

std::span<const std::uint32_t> GridFilter::active_on(int level) const {
  if (!multi_level_) return active_;
  const std::size_t begin = map_->level_begin(level), end = map_->level_begin(level + 1);
  if (!sparse_) return {active_.data() + begin, end - begin};
  const auto first = std::lower_bound(active_.begin(), active_.end(), static_cast<std::uint32_t>(begin));
  const auto last = std::lower_bound(first, active_.end(), static_cast<std::uint32_t>(end));
  return {active_.data() + (first - active_.begin()), static_cast<std::size_t>(last - first)};
}

void GridFilter::activate_block(std::size_t block) {
  // No update has told the block's cells apart: its mass comes back evenly over them, at their centres,
  // with the block's heading row and offsets.
  const auto cells = map_->block_cells(block);
  const double share = bg_mass_[block] / static_cast<double>(cells.size());
  const auto row = static_cast<std::size_t>(bins_);
  for (const std::uint32_t c : cells) {
    reset_cell_to_prior(c, share);
    dev_mu_[c] = bg_dev_mu_[block];
    dev_var_[c] = bg_dev_var_[block];
    baro_mu_[c] = bg_baro_mu_[block];
    baro_var_[c] = bg_baro_var_[block];
    if (sparse_) std::copy_n(&bg_h_[block * row], row, &h_[c * row]);
    cell_active_[c] = 1;
  }
  bg_mass_[block] = 0.0;
  block_active_[block] = 1;
}

void GridFilter::fold_block(std::size_t block) {
  // The block's offsets and heading row are those of its cells' mass (moment matching, as a step does for
  // a cell).
  const auto row = static_cast<std::size_t>(bins_);
  double sum = 0.0, d1 = 0.0, d2 = 0.0, b1 = 0.0, b2 = 0.0, sx = 0.0, sy = 0.0;
  std::fill(row_.begin(), row_.end(), 0.0);
  for (const std::uint32_t c : map_->block_cells(block)) {
    const double m = std::max(mass_[c], 0.0);
    sum += m;
    d1 += m * dev_mu_[c];
    d2 += m * (dev_var_[c] + dev_mu_[c] * dev_mu_[c]);
    b1 += m * baro_mu_[c];
    b2 += m * (baro_var_[c] + baro_mu_[c] * baro_mu_[c]);
    sx += m * cx_[c];
    sy += m * cy_[c];
    if (m > 0.0) {
      const float* hc = &h_[c * row];
      for (std::size_t k = 0; k < row; ++k) row_[k] += m * hc[k];
    }
    mass_[c] = 0.0;
    cell_active_[c] = 0;
  }
  bg_mass_[block] = sum;
  float* const hb = &bg_h_[block * row];
  if (sum > 0.0) {
    bg_dev_mu_[block] = d1 / sum;
    bg_dev_var_[block] =
        cfg_.estimate_rssi_offset ? std::max(d2 / sum - bg_dev_mu_[block] * bg_dev_mu_[block], kMinVar) : 0.0;
    bg_baro_mu_[block] = b1 / sum;
    bg_baro_var_[block] = std::max(b2 / sum - bg_baro_mu_[block] * bg_baro_mu_[block], kMinVar);
    bg_x_[block] = sx / sum;
    bg_y_[block] = sy / sum;
    double hsum = 0.0;
    for (std::size_t k = 0; k < row; ++k) hsum += row_[k];
    for (std::size_t k = 0; k < row; ++k) {
      hb[k] = hsum > 0.0 ? static_cast<float>(row_[k] / hsum) : static_cast<float>(1.0 / bins_);
    }
  } else {
    bg_dev_mu_[block] = 0.0;
    bg_dev_var_[block] = prior_rssi_offset_var();
    bg_baro_mu_[block] = prior_baro_offset(map_->block_level(block));
    bg_baro_var_[block] = prior_baro_offset_var();
    bg_x_[block] = map_->block_centre(block).x;
    bg_y_[block] = map_->block_centre(block).y;
    std::fill_n(hb, row, static_cast<float>(1.0 / bins_));
  }
  block_active_[block] = 0;
}

void GridFilter::activate_all() {
  for (std::size_t b = 0; b < block_active_.size(); ++b) {
    if (block_active_[b] == 0) activate_block(b);
  }
  std::fill(block_edge_.begin(), block_edge_.end(), std::uint8_t{0});
  active_.resize(map_->size());
  std::iota(active_.begin(), active_.end(), std::uint32_t{0});
}

void GridFilter::rebuild_active_cells() {
  active_.clear();
  for (std::size_t b = 0; b < block_active_.size(); ++b) {
    if (block_active_[b] == 0) continue;
    const auto cells = map_->block_cells(b);
    active_.insert(active_.end(), cells.begin(), cells.end());
  }
  // Index order: the order every sum over cells is taken in, with and without the active set.
  std::sort(active_.begin(), active_.end());
}

void GridFilter::refresh_active_set() {
  if (!sparse_) return;
  const std::size_t blocks = map_->block_count();
  // The reference is the heaviest cell, counting a background block as cells with its mass spread evenly.
  double ref = 0.0;
  for (const std::uint32_t c : active_) ref = std::max(ref, mass_[c]);
  for (std::size_t b = 0; b < blocks; ++b) {
    if (block_active_[b] == 0) ref = std::max(ref, bg_mass_[b] / static_cast<double>(map_->block_cells(b).size()));
  }
  if (!(ref > 0.0)) return;
  const double floor = active_floor_ * ref;

  // 1: the block holds a cell at or above the floor. 2: it is next to one, so that no step and no level
  // change takes mass above the floor into the background.
  std::fill(block_mark_.begin(), block_mark_.end(), std::uint8_t{0});
  for (const std::uint32_t c : active_) {
    if (mass_[c] >= floor) block_mark_[map_->block_of(c)] = 1;
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    if (block_active_[b] == 0 &&
        bg_mass_[b] >= kActivateRatio * floor * static_cast<double>(map_->block_cells(b).size())) {
      block_mark_[b] = 1;
    }
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    if (block_mark_[b] != 1) continue;
    for (const std::uint32_t nb : map_->block_neighbours(b)) {
      if (block_mark_[nb] == 0) block_mark_[nb] = 2;
    }
  }
  bool changed = false;
  for (std::size_t b = 0; b < blocks; ++b) {
    const bool want = block_mark_[b] != 0;
    if (want == (block_active_[b] != 0)) continue;
    if (want) {
      activate_block(b);
    } else {
      fold_block(b);
    }
    changed = true;
  }
  if (!changed) return;
  for (std::size_t b = 0; b < blocks; ++b) {
    block_edge_[b] = 0;
    if (block_active_[b] == 0) continue;
    for (const std::uint32_t nb : map_->block_neighbours(b)) {
      if (block_active_[nb] == 0) block_edge_[b] = 1;
    }
  }
  rebuild_active_cells();
}

}  // namespace ipe
