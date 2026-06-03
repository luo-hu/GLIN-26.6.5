// Fig.15/Fig.16 style update benchmark for WKT datasets.
//
// Insert workload: bulk-load a random 50% of the records, then insert the
// remaining records one by one.
// Delete workload: bulk-load 100% of the records, then delete a random 50%
// of the records one by one.

#include "../../glin/glin.h"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/index/quadtree/Quadtree.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
using Point = bg::model::point<double, 2, bg::cs::cartesian>;
using Box = bg::model::box<Point>;
using RTreeValue = std::pair<Box, std::size_t>;
using RTreeLinear = bgi::rtree<RTreeValue, bgi::linear<16>>;
using RTreeQuadratic = bgi::rtree<RTreeValue, bgi::quadratic<16>>;
using RTreeRStar = bgi::rtree<RTreeValue, bgi::rstar<16>>;
using Quadtree = geos::index::quadtree::Quadtree;

struct Options {
  std::string data_file;
  std::string dataset_name = "WKT";
  std::string mode = "both";
  std::string output_csv;
  std::string progress_csv;
  std::string boost_strategy = "linear";
  std::string insert_order = "random";
  std::size_t limit = 10000;
  std::uint64_t seed = 42;
  double initial_fraction = 0.5;
  double delete_fraction = 0.5;
  double piece_limit = 10000.0;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  std::size_t progress_step_percent = 10;
  bool include_baselines = true;
  bool append_csv = false;
};

struct LoadStats {
  std::size_t lines_seen = 0;
  std::size_t parse_errors = 0;
  long long load_ns = 0;
};

struct UpdateResult {
  std::string index;
  std::string operation;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t update_count = 0;
  long long build_ns = 0;
  long long update_ns = 0;
  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  std::size_t pieces = 0;
};

struct ProgressResult {
  std::string index;
  std::string operation;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t total_update_count = 0;
  std::size_t checkpoint_update_count = 0;
  double update_percent = 0.0;
  long long build_ns = 0;
  long long elapsed_update_ns = 0;
  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  std::size_t pieces = 0;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file /path/to/data.wkt [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME      Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N                Number of valid geometries to load (default: 10000)\n"
      << "  --mode insert|delete|both  Update workload to run (default: both)\n"
      << "  --initial_fraction F     Bulk-loaded fraction for insert workload (default: 0.5)\n"
      << "  --delete_fraction F      Deleted fraction for delete workload (default: 0.5)\n"
      << "  --insert_order random|file|zmin  Record order for insert workload (default: random)\n"
      << "  --boost_strategy linear|quadratic|rstar|all  Boost R-tree split strategy (default: linear)\n"
      << "  --include_baselines 0|1  Run Boost/Quadtree in non-PIECE target (default: 1)\n"
      << "  --seed N                 Random seed for shuffling records (default: 42)\n"
      << "  --piece_limit N          Records per piece for PIECE build (default: 10000)\n"
      << "  --cell_xmin X            Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y            Z-order latitude origin (default: -90)\n"
      << "  --cell_size S            Z-order cell size for x/y (default: 5e-7)\n"
      << "  --output_csv PATH        Write summary CSV rows\n"
      << "  --progress_csv PATH      Write checkpoint CSV rows for curve plots\n"
      << "  --progress_step_percent N  Checkpoint step percent (default: 10)\n"
      << "  --append_csv             Append to output CSV instead of overwriting\n";
}

Options parse_args(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("Missing value for " + flag);
      }
      return argv[++i];
    };

    if (key == "--data_file") {
      options.data_file = require_value(key);
    } else if (key == "--dataset_name") {
      options.dataset_name = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--mode") {
      options.mode = require_value(key);
    } else if (key == "--initial_fraction") {
      options.initial_fraction = std::stod(require_value(key));
    } else if (key == "--delete_fraction") {
      options.delete_fraction = std::stod(require_value(key));
    } else if (key == "--insert_order") {
      options.insert_order = require_value(key);
    } else if (key == "--boost_strategy") {
      options.boost_strategy = require_value(key);
    } else if (key == "--include_baselines") {
      options.include_baselines = (require_value(key) != "0");
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--piece_limit") {
      options.piece_limit = std::stod(require_value(key));
    } else if (key == "--cell_xmin") {
      options.cell_xmin = std::stod(require_value(key));
    } else if (key == "--cell_ymin") {
      options.cell_ymin = std::stod(require_value(key));
    } else if (key == "--cell_size") {
      options.cell_size = std::stod(require_value(key));
    } else if (key == "--output_csv") {
      options.output_csv = require_value(key);
    } else if (key == "--progress_csv") {
      options.progress_csv = require_value(key);
    } else if (key == "--progress_step_percent") {
      options.progress_step_percent =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--append_csv") {
      options.append_csv = true;
    } else if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }

  if (options.data_file.empty()) {
    throw std::runtime_error("--data_file is required");
  }
  if (options.limit == 0) {
    throw std::runtime_error("--limit must be greater than 0");
  }
  if (options.mode != "insert" && options.mode != "delete" &&
      options.mode != "both") {
    throw std::runtime_error("--mode must be insert, delete, or both");
  }
  if (options.insert_order != "random" && options.insert_order != "file" &&
      options.insert_order != "zmin") {
    throw std::runtime_error("--insert_order must be random, file, or zmin");
  }
  if (options.boost_strategy != "linear" &&
      options.boost_strategy != "quadratic" &&
      options.boost_strategy != "rstar" && options.boost_strategy != "all") {
    throw std::runtime_error(
        "--boost_strategy must be linear, quadratic, rstar, or all");
  }
  if (options.initial_fraction <= 0.0 || options.initial_fraction >= 1.0) {
    throw std::runtime_error("--initial_fraction must be between 0 and 1");
  }
  if (options.delete_fraction <= 0.0 || options.delete_fraction > 1.0) {
    throw std::runtime_error("--delete_fraction must be between 0 and 1");
  }
  if (options.progress_step_percent == 0 ||
      options.progress_step_percent > 100) {
    throw std::runtime_error("--progress_step_percent must be between 1 and 100");
  }
  return options;
}

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void strip_bom(std::string& line) {
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
}

void strip_cr(std::string& line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
}

std::string extract_balanced_wkt(const std::string& text, std::size_t start) {
  std::size_t open = text.find('(', start);
  if (open == std::string::npos) {
    return text.substr(start);
  }

  int depth = 0;
  for (std::size_t i = open; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      --depth;
      if (depth == 0) {
        return text.substr(start, i - start + 1);
      }
    }
  }
  return text.substr(start);
}

std::string extract_wkt(std::string line) {
  strip_bom(line);
  strip_cr(line);
  if (line.empty()) {
    return "";
  }

  if (line.front() == '"') {
    std::size_t close_quote = line.find('"', 1);
    if (close_quote != std::string::npos) {
      return line.substr(1, close_quote - 1);
    }
  }

  static const std::vector<std::string> prefixes = {
      "GEOMETRYCOLLECTION", "MULTIPOLYGON", "POLYGON", "MULTILINESTRING",
      "LINESTRING", "MULTIPOINT", "POINT"};
  for (const auto& prefix : prefixes) {
    std::size_t pos = line.find(prefix);
    if (pos != std::string::npos) {
      return extract_balanced_wkt(line, pos);
    }
  }
  return line;
}

std::vector<GeometryPtr> load_wkt_csv(const Options& options,
                                      geos::io::WKTReader& reader,
                                      LoadStats& stats) {
  std::ifstream input(options.data_file);
  if (!input) {
    throw std::runtime_error("Cannot open data file: " + options.data_file);
  }

  std::vector<GeometryPtr> geometries;
  geometries.reserve(options.limit);
  std::string line;
  std::size_t first_error_line = 0;
  std::string first_error_message;

  auto load_start = std::chrono::high_resolution_clock::now();
  while (geometries.size() < options.limit && std::getline(input, line)) {
    ++stats.lines_seen;
    std::string wkt = extract_wkt(line);
    if (wkt.empty()) {
      continue;
    }

    try {
      GeometryPtr geometry(reader.read(wkt));
      if (geometry && !geometry->isEmpty()) {
        geometries.push_back(std::move(geometry));
      }
    } catch (const geos::util::GEOSException& ex) {
      ++stats.parse_errors;
      if (first_error_line == 0) {
        first_error_line = stats.lines_seen;
        first_error_message = ex.what();
      }
    } catch (const std::exception& ex) {
      ++stats.parse_errors;
      if (first_error_line == 0) {
        first_error_line = stats.lines_seen;
        first_error_message = ex.what();
      }
    }
  }
  auto load_end = std::chrono::high_resolution_clock::now();
  stats.load_ns = ns_count(load_end - load_start);

  if (first_error_line != 0) {
    std::cerr << "First parse error at line " << first_error_line << ": "
              << first_error_message << "\n";
  }
  return geometries;
}

std::vector<std::size_t> shuffled_ids(std::size_t n, std::uint64_t seed) {
  std::vector<std::size_t> ids(n);
  std::iota(ids.begin(), ids.end(), 0);
  std::mt19937_64 rng(seed);
  std::shuffle(ids.begin(), ids.end(), rng);
  return ids;
}

std::vector<std::size_t> file_order_ids(std::size_t n) {
  std::vector<std::size_t> ids(n);
  std::iota(ids.begin(), ids.end(), 0);
  return ids;
}

std::vector<std::size_t> zmin_order_ids(const Options& options,
                                        const std::vector<GeometryPtr>& geometries) {
  std::vector<std::pair<double, std::size_t>> keyed_ids;
  keyed_ids.reserve(geometries.size());
  for (std::size_t i = 0; i < geometries.size(); ++i) {
    double zmin = 0.0;
    double zmax = 0.0;
    curve_shape_projection(geometries[i].get(), "z", options.cell_xmin,
                           options.cell_ymin, options.cell_size,
                           options.cell_size, zmin, zmax);
    keyed_ids.emplace_back(zmin, i);
  }
  std::sort(keyed_ids.begin(), keyed_ids.end(),
            [](const std::pair<double, std::size_t>& a,
               const std::pair<double, std::size_t>& b) {
              if (a.first != b.first) {
                return a.first < b.first;
              }
              return a.second < b.second;
            });

  std::vector<std::size_t> ids;
  ids.reserve(keyed_ids.size());
  for (const auto& keyed_id : keyed_ids) {
    ids.push_back(keyed_id.second);
  }
  return ids;
}

std::vector<std::size_t> make_ordered_ids(
    const Options& options, const std::vector<GeometryPtr>& geometries) {
  if (options.insert_order == "file") {
    return file_order_ids(geometries.size());
  }
  if (options.insert_order == "zmin") {
    return zmin_order_ids(options, geometries);
  }
  return shuffled_ids(geometries.size(), options.seed);
}

std::vector<Geometry*> raw_ptrs_by_ids(const std::vector<GeometryPtr>& geometries,
                                       const std::vector<std::size_t>& ids,
                                       std::size_t begin,
                                       std::size_t end) {
  std::vector<Geometry*> raw;
  raw.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    raw.push_back(geometries[ids[i]].get());
  }
  return raw;
}

Box box_from_envelope(const geos::geom::Envelope* envelope) {
  return Box(Point(envelope->getMinX(), envelope->getMinY()),
             Point(envelope->getMaxX(), envelope->getMaxY()));
}

template <typename RTreeType>
RTreeType build_rtree_by_ids(const std::vector<GeometryPtr>& geometries,
                             const std::vector<std::size_t>& ids,
                             std::size_t count) {
  std::vector<RTreeValue> values;
  values.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::size_t id = ids[i];
    values.emplace_back(box_from_envelope(geometries[id]->getEnvelopeInternal()), id);
  }
  return RTreeType(values.begin(), values.end());
}

void build_quadtree_by_ids(const std::vector<GeometryPtr>& geometries,
                           const std::vector<std::size_t>& ids,
                           std::size_t count,
                           Quadtree& quadtree) {
  for (std::size_t i = 0; i < count; ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    quadtree.insert(geometry->getEnvelopeInternal(), geometry);
  }
}

std::vector<std::size_t> make_progress_checkpoints(std::size_t loaded_count,
                                                   std::size_t update_count,
                                                   std::size_t step_percent) {
  std::vector<std::size_t> checkpoints;
  for (std::size_t percent = step_percent; percent <= 100;
       percent += step_percent) {
    std::size_t checkpoint =
        (loaded_count * percent + 99) / 100;
    if (checkpoint == 0) {
      continue;
    }
    if (checkpoint > update_count) {
      break;
    }
    if (checkpoints.empty() || checkpoints.back() != checkpoint) {
      checkpoints.push_back(checkpoint);
    }
  }
  if (checkpoints.empty() || checkpoints.back() != update_count) {
    checkpoints.push_back(update_count);
  }
  return checkpoints;
}

void capture_progress(const Options& options,
                      std::vector<ProgressResult>& progress,
                      const std::string& index_name,
                      const std::string& operation,
                      std::size_t loaded_count,
                      std::size_t initial_count,
                      std::size_t total_update_count,
                      long long build_ns,
                      const std::vector<std::size_t>& checkpoints,
                      std::size_t& next_checkpoint,
                      std::chrono::high_resolution_clock::time_point update_start,
                      std::size_t updates_done,
                      std::size_t success_count,
                      std::size_t failed_count,
                      std::size_t pieces) {
  if (options.progress_csv.empty()) {
    return;
  }

  while (next_checkpoint < checkpoints.size() &&
         updates_done >= checkpoints[next_checkpoint]) {
    auto now = std::chrono::high_resolution_clock::now();
    ProgressResult row;
    row.index = index_name;
    row.operation = operation;
    row.loaded_count = loaded_count;
    row.initial_count = initial_count;
    row.total_update_count = total_update_count;
    row.checkpoint_update_count = checkpoints[next_checkpoint];
    row.update_percent =
        100.0 * static_cast<double>(row.checkpoint_update_count) /
        static_cast<double>(loaded_count);
    row.build_ns = build_ns;
    row.elapsed_update_ns = ns_count(now - update_start);
    row.success_count = success_count;
    row.failed_count = failed_count;
    row.pieces = pieces;
    progress.push_back(row);
    ++next_checkpoint;
  }
}

UpdateResult run_glin_insert(const Options& options,
                             const std::vector<GeometryPtr>& geometries,
                             const std::vector<std::size_t>& ids,
                             std::size_t initial_count,
                             std::vector<ProgressResult>& progress) {
  alex::Glin<double, Geometry*> index;
  std::vector<std::tuple<double, double, double, double>> pieces;

  std::vector<Geometry*> initial_geometries =
      raw_ptrs_by_ids(geometries, ids, 0, initial_count);
  auto build_start = std::chrono::high_resolution_clock::now();
  index.glin_bulk_load(initial_geometries, options.piece_limit, "z",
                       options.cell_xmin, options.cell_ymin,
                       options.cell_size, options.cell_size, pieces);
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);

#ifdef PIECE
  const std::string index_name = "GLIN_PIECEWISE";
#else
  const std::string index_name = "GLIN";
#endif
  const std::size_t update_count = geometries.size() - initial_count;
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), update_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = initial_count; i < geometries.size(); ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    geos::geom::Envelope* envelope =
        const_cast<geos::geom::Envelope*>(geometry->getEnvelopeInternal());
    auto result = index.glin_insert(
        std::make_tuple(geometry, envelope), "z",
        options.cell_xmin, options.cell_ymin, options.cell_size,
        options.cell_size, options.piece_limit, pieces);
    if (result.second) {
      ++success_count;
    } else {
      ++failed_count;
    }
    std::size_t updates_done = i - initial_count + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, index_name, "insert",
                       geometries.size(), initial_count, update_count, build_ns,
                       checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, failed_count, pieces.size());
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = index_name;
  result.operation = "insert";
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.update_count = update_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = success_count;
  result.failed_count = failed_count;
  result.pieces = pieces.size();
  return result;
}

UpdateResult run_glin_delete(const Options& options,
                             const std::vector<GeometryPtr>& geometries,
                             const std::vector<std::size_t>& ids,
                             std::size_t delete_count,
                             std::vector<ProgressResult>& progress) {
  alex::Glin<double, Geometry*> index;
  std::vector<std::tuple<double, double, double, double>> pieces;

  std::vector<Geometry*> all_geometries =
      raw_ptrs_by_ids(geometries, ids, 0, geometries.size());
  auto build_start = std::chrono::high_resolution_clock::now();
  index.glin_bulk_load(all_geometries, options.piece_limit, "z",
                       options.cell_xmin, options.cell_ymin,
                       options.cell_size, options.cell_size, pieces);
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);

#ifdef PIECE
  const std::string index_name = "GLIN_PIECEWISE";
#else
  const std::string index_name = "GLIN";
#endif
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), delete_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = 0; i < delete_count; ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    int erased = index.erase(geometry, "z", options.cell_xmin,
                             options.cell_ymin, options.cell_size,
                             options.cell_size, options.piece_limit, pieces);
    if (erased > 0) {
      ++success_count;
    } else {
      ++failed_count;
    }
    std::size_t updates_done = i + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, index_name, "delete",
                       geometries.size(), geometries.size(), delete_count,
                       build_ns, checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, failed_count, pieces.size());
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = index_name;
  result.operation = "delete";
  result.loaded_count = geometries.size();
  result.initial_count = geometries.size();
  result.update_count = delete_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = success_count;
  result.failed_count = failed_count;
  result.pieces = pieces.size();
  return result;
}

template <typename RTreeType>
UpdateResult run_boost_insert(const Options& options,
                              const std::string& index_name,
                              const std::vector<GeometryPtr>& geometries,
                              const std::vector<std::size_t>& ids,
                              std::size_t initial_count,
                              std::vector<ProgressResult>& progress) {
  auto build_start = std::chrono::high_resolution_clock::now();
  RTreeType rtree = build_rtree_by_ids<RTreeType>(geometries, ids, initial_count);
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);
  const std::size_t update_count = geometries.size() - initial_count;
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), update_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = initial_count; i < geometries.size(); ++i) {
    std::size_t id = ids[i];
    rtree.insert(
        std::make_pair(box_from_envelope(geometries[id]->getEnvelopeInternal()), id));
    ++success_count;
    std::size_t updates_done = i - initial_count + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, index_name, "insert",
                       geometries.size(), initial_count, update_count, build_ns,
                       checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, 0, 0);
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = index_name;
  result.operation = "insert";
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.update_count = update_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = result.update_count;
  return result;
}

template <typename RTreeType>
UpdateResult run_boost_delete(const Options& options,
                              const std::string& index_name,
                              const std::vector<GeometryPtr>& geometries,
                              const std::vector<std::size_t>& ids,
                              std::size_t delete_count,
                              std::vector<ProgressResult>& progress) {
  auto build_start = std::chrono::high_resolution_clock::now();
  RTreeType rtree = build_rtree_by_ids<RTreeType>(geometries, ids, geometries.size());
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), delete_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = 0; i < delete_count; ++i) {
    std::size_t id = ids[i];
    std::size_t removed = rtree.remove(
        std::make_pair(box_from_envelope(geometries[id]->getEnvelopeInternal()), id));
    if (removed > 0) {
      ++success_count;
    } else {
      ++failed_count;
    }
    std::size_t updates_done = i + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, index_name, "delete",
                       geometries.size(), geometries.size(), delete_count,
                       build_ns, checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, failed_count, 0);
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = index_name;
  result.operation = "delete";
  result.loaded_count = geometries.size();
  result.initial_count = geometries.size();
  result.update_count = delete_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = success_count;
  result.failed_count = failed_count;
  return result;
}

UpdateResult run_quadtree_insert(const Options& options,
                                 const std::vector<GeometryPtr>& geometries,
                                 const std::vector<std::size_t>& ids,
                                 std::size_t initial_count,
                                 std::vector<ProgressResult>& progress) {
  Quadtree quadtree;
  auto build_start = std::chrono::high_resolution_clock::now();
  build_quadtree_by_ids(geometries, ids, initial_count, quadtree);
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);
  const std::size_t update_count = geometries.size() - initial_count;
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), update_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = initial_count; i < geometries.size(); ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    quadtree.insert(geometry->getEnvelopeInternal(), geometry);
    ++success_count;
    std::size_t updates_done = i - initial_count + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, "GEOS_Quadtree", "insert",
                       geometries.size(), initial_count, update_count, build_ns,
                       checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, 0, 0);
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = "GEOS_Quadtree";
  result.operation = "insert";
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.update_count = update_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = result.update_count;
  return result;
}

UpdateResult run_quadtree_delete(const Options& options,
                                 const std::vector<GeometryPtr>& geometries,
                                 const std::vector<std::size_t>& ids,
                                 std::size_t delete_count,
                                 std::vector<ProgressResult>& progress) {
  Quadtree quadtree;
  auto build_start = std::chrono::high_resolution_clock::now();
  build_quadtree_by_ids(geometries, ids, geometries.size(), quadtree);
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);
  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), delete_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;

  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  auto update_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = 0; i < delete_count; ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    if (quadtree.remove(geometry->getEnvelopeInternal(), geometry)) {
      ++success_count;
    } else {
      ++failed_count;
    }
    std::size_t updates_done = i + 1;
    if (!options.progress_csv.empty() &&
        next_checkpoint < checkpoints.size() &&
        updates_done >= checkpoints[next_checkpoint]) {
      capture_progress(options, progress, "GEOS_Quadtree", "delete",
                       geometries.size(), geometries.size(), delete_count,
                       build_ns, checkpoints, next_checkpoint, update_start,
                       updates_done, success_count, failed_count, 0);
    }
  }
  auto update_end = std::chrono::high_resolution_clock::now();

  UpdateResult result;
  result.index = "GEOS_Quadtree";
  result.operation = "delete";
  result.loaded_count = geometries.size();
  result.initial_count = geometries.size();
  result.update_count = delete_count;
  result.build_ns = build_ns;
  result.update_ns = ns_count(update_end - update_start);
  result.success_count = success_count;
  result.failed_count = failed_count;
  return result;
}

std::string boost_index_name(const Options& options,
                             const std::string& strategy) {
  if (options.boost_strategy == "all") {
    return "Boost_Rtree_" + strategy;
  }
  if (strategy == "linear") {
    return "Boost_Rtree";
  }
  return "Boost_Rtree_" + strategy;
}

void run_boost_insert_for_strategy(
    const Options& options,
    const std::string& strategy,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<std::size_t>& ids,
    std::size_t initial_count,
    std::vector<ProgressResult>& progress,
    std::vector<UpdateResult>& results) {
  std::string index_name = boost_index_name(options, strategy);
  if (strategy == "linear") {
    results.push_back(run_boost_insert<RTreeLinear>(
        options, index_name, geometries, ids, initial_count, progress));
  } else if (strategy == "quadratic") {
    results.push_back(run_boost_insert<RTreeQuadratic>(
        options, index_name, geometries, ids, initial_count, progress));
  } else if (strategy == "rstar") {
    results.push_back(run_boost_insert<RTreeRStar>(
        options, index_name, geometries, ids, initial_count, progress));
  }
}

void run_boost_delete_for_strategy(
    const Options& options,
    const std::string& strategy,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<std::size_t>& ids,
    std::size_t delete_count,
    std::vector<ProgressResult>& progress,
    std::vector<UpdateResult>& results) {
  std::string index_name = boost_index_name(options, strategy);
  if (strategy == "linear") {
    results.push_back(run_boost_delete<RTreeLinear>(
        options, index_name, geometries, ids, delete_count, progress));
  } else if (strategy == "quadratic") {
    results.push_back(run_boost_delete<RTreeQuadratic>(
        options, index_name, geometries, ids, delete_count, progress));
  } else if (strategy == "rstar") {
    results.push_back(run_boost_delete<RTreeRStar>(
        options, index_name, geometries, ids, delete_count, progress));
  }
}

std::vector<std::string> selected_boost_strategies(const Options& options) {
  if (options.boost_strategy == "all") {
    return {"linear", "quadratic", "rstar"};
  }
  return {options.boost_strategy};
}

bool output_has_content(const std::string& path) {
  std::ifstream input(path);
  return input.good() && input.peek() != std::ifstream::traits_type::eof();
}

void write_csv(const std::string& path, const Options& options,
               const LoadStats& stats,
               const std::vector<UpdateResult>& results) {
  if (path.empty()) {
    return;
  }

  bool write_header = true;
  if (options.append_csv && output_has_content(path)) {
    write_header = false;
  }

  std::ios_base::openmode mode = std::ios::out;
  if (options.append_csv) {
    mode |= std::ios::app;
  }

  std::ofstream output(path, mode);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }

  if (write_header) {
    output << "dataset,index,operation,loaded_count,initial_count,update_count,"
              "insert_order,boost_strategy,piece_limit,cell_xmin,cell_ymin,cell_size,seed,lines_seen,"
              "parse_errors,load_ns,build_ns,update_ns,avg_update_ns,"
              "throughput_ops_per_sec,success_count,failed_count,pieces\n";
  }

  for (const auto& result : results) {
    double avg_update_ns = result.update_count == 0
                               ? 0.0
                               : static_cast<double>(result.update_ns) /
                                     static_cast<double>(result.update_count);
    double throughput = result.update_ns == 0
                            ? 0.0
                            : static_cast<double>(result.update_count) * 1e9 /
                                  static_cast<double>(result.update_ns);
    output << options.dataset_name << "," << result.index << ","
           << result.operation << "," << result.loaded_count << ","
           << result.initial_count << "," << result.update_count << ","
           << options.insert_order << "," << options.boost_strategy << ","
           << options.piece_limit << "," << options.cell_xmin << ","
           << options.cell_ymin << "," << options.cell_size << ","
           << options.seed << "," << stats.lines_seen << ","
           << stats.parse_errors << "," << stats.load_ns << ","
           << result.build_ns << "," << result.update_ns << ","
           << avg_update_ns << "," << throughput << ","
           << result.success_count << "," << result.failed_count << ","
           << result.pieces << "\n";
  }
}

void write_progress_csv(const std::string& path, const Options& options,
                        const LoadStats& stats,
                        const std::vector<ProgressResult>& results) {
  if (path.empty()) {
    return;
  }

  bool write_header = true;
  if (options.append_csv && output_has_content(path)) {
    write_header = false;
  }

  std::ios_base::openmode mode = std::ios::out;
  if (options.append_csv) {
    mode |= std::ios::app;
  }

  std::ofstream output(path, mode);
  if (!output) {
    throw std::runtime_error("Cannot open progress CSV: " + path);
  }

  if (write_header) {
    output << "dataset,index,operation,loaded_count,initial_count,"
              "total_update_count,checkpoint_update_count,update_percent,"
              "insert_order,boost_strategy,piece_limit,cell_xmin,cell_ymin,cell_size,seed,lines_seen,"
              "parse_errors,load_ns,build_ns,elapsed_update_ns,"
              "throughput_records_per_sec,success_count,failed_count,pieces\n";
  }

  for (const auto& result : results) {
    double throughput = result.elapsed_update_ns == 0
                            ? 0.0
                            : static_cast<double>(result.checkpoint_update_count) *
                                  1e9 /
                                  static_cast<double>(result.elapsed_update_ns);
    output << options.dataset_name << "," << result.index << ","
           << result.operation << "," << result.loaded_count << ","
           << result.initial_count << "," << result.total_update_count << ","
           << result.checkpoint_update_count << "," << result.update_percent
           << "," << options.insert_order << "," << options.boost_strategy
           << "," << options.piece_limit << "," << options.cell_xmin << ","
           << options.cell_ymin << "," << options.cell_size << ","
           << options.seed << "," << stats.lines_seen << ","
           << stats.parse_errors << "," << stats.load_ns << ","
           << result.build_ns << "," << result.elapsed_update_ns << ","
           << throughput << "," << result.success_count << ","
           << result.failed_count << "," << result.pieces << "\n";
  }
}

void print_result(const Options& options, const LoadStats& stats,
                  const UpdateResult& result) {
  double avg_update_ns = result.update_count == 0
                             ? 0.0
                             : static_cast<double>(result.update_ns) /
                                   static_cast<double>(result.update_count);
  double throughput = result.update_ns == 0
                          ? 0.0
                          : static_cast<double>(result.update_count) * 1e9 /
                                static_cast<double>(result.update_ns);
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "dataset=" << options.dataset_name
            << " index=" << result.index
            << " operation=" << result.operation
            << " loaded=" << result.loaded_count
            << " initial=" << result.initial_count
            << " updates=" << result.update_count
            << " success=" << result.success_count
            << " failed=" << result.failed_count
            << " pieces=" << result.pieces
            << " lines_seen=" << stats.lines_seen
            << " parse_errors=" << stats.parse_errors
            << " load_ms=" << stats.load_ns / 1e6
            << " build_ms=" << result.build_ns / 1e6
            << " update_ms=" << result.update_ns / 1e6
            << " avg_update_ns=" << avg_update_ns
            << " throughput_ops_per_sec=" << throughput << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    LoadStats stats;
    std::vector<GeometryPtr> geometries = load_wkt_csv(options, reader, stats);
    if (geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }
    if (geometries.size() < 2) {
      throw std::runtime_error("Need at least two valid geometries");
    }

    std::vector<std::size_t> ids = make_ordered_ids(options, geometries);
    std::size_t initial_count = static_cast<std::size_t>(
        static_cast<double>(geometries.size()) * options.initial_fraction);
    initial_count = std::max<std::size_t>(1, std::min(initial_count, geometries.size() - 1));
    std::size_t delete_count = static_cast<std::size_t>(
        static_cast<double>(geometries.size()) * options.delete_fraction);
    delete_count = std::max<std::size_t>(1, std::min(delete_count, geometries.size()));

    std::vector<UpdateResult> results;
    std::vector<ProgressResult> progress_results;

    if (options.mode == "insert" || options.mode == "both") {
      results.push_back(run_glin_insert(options, geometries, ids, initial_count,
                                        progress_results));
#ifndef PIECE
      if (options.include_baselines) {
        for (const auto& strategy : selected_boost_strategies(options)) {
          run_boost_insert_for_strategy(options, strategy, geometries, ids,
                                        initial_count, progress_results, results);
        }
        results.push_back(run_quadtree_insert(options, geometries, ids, initial_count,
                                              progress_results));
      }
#endif
    }

    if (options.mode == "delete" || options.mode == "both") {
      results.push_back(run_glin_delete(options, geometries, ids, delete_count,
                                        progress_results));
#ifndef PIECE
      if (options.include_baselines) {
        for (const auto& strategy : selected_boost_strategies(options)) {
          run_boost_delete_for_strategy(options, strategy, geometries, ids,
                                        delete_count, progress_results, results);
        }
        results.push_back(run_quadtree_delete(options, geometries, ids, delete_count,
                                              progress_results));
      }
#endif
    }

    write_csv(options.output_csv, options, stats, results);
    write_progress_csv(options.progress_csv, options, stats, progress_results);
    for (const auto& result : results) {
      print_result(options, stats, result);
    }
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
