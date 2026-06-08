// Fig.17 style hybrid workload benchmark for WKT datasets.
//
// Protocol used by the GLIN arXiv paper:
//   1. Bulk-load 50% of the dataset.
//   2. A transaction is either:
//      - one 1% Intersects range query, or
//      - insertion of 1% new records.
//   3. Read-intensive workload: 90% query transactions, 10% insert transactions.
//      Write-intensive workload: 50% query transactions, 50% insert transactions.
//   4. Stop when the remaining 50% records have been inserted.

#ifndef PIECE
#error "bench_hybrid_wkt.cpp must be compiled with PIECE for Intersects workloads"
#endif

#include "../../glin/glin.h"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/geom/LinearRing.h>
#include <geos/index/quadtree/Quadtree.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
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
using RTree = bgi::rtree<RTreeValue, bgi::linear<16>>;
using Quadtree = geos::index::quadtree::Quadtree;

struct Options {
  std::string data_file;
  std::string query_file;
  std::string dataset_name = "WKT";
  std::string workload = "both";
  std::string output_csv;
  std::string progress_csv;
  std::string insert_order = "random";
  std::size_t limit = 10000;
  std::uint64_t seed = 42;
  double initial_fraction = 0.5;
  double insert_fraction = 0.5;
  double insert_batch_percent = 0.01;
  double piece_limit = 10000.0;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  std::size_t progress_step_percent = 10;
  std::size_t delta_size = 100000;
  bool include_lsm_async_glin = false;
  bool append_csv = false;
};

struct LoadStats {
  std::size_t lines_seen = 0;
  std::size_t parse_errors = 0;
  long long load_ns = 0;
};

struct QueryCase {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  geos::geom::Envelope envelope;
  Geometry* geometry = nullptr;
};

struct HybridResult {
  std::string index;
  std::string workload;
  double query_fraction = 0.0;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t total_insert_count = 0;
  std::size_t transaction_count = 0;
  std::size_t query_transactions = 0;
  std::size_t insert_transactions = 0;
  std::size_t insert_success_count = 0;
  std::size_t insert_failed_count = 0;
  long long build_ns = 0;
  long long workload_ns = 0;
  long long maintenance_ns = 0;
  std::size_t pieces = 0;
  std::size_t merge_count = 0;
  std::size_t delta_pending = 0;
  long long candidates_total = 0;
  long long answers_total = 0;
};

struct ProgressResult {
  std::string index;
  std::string workload;
  double query_fraction = 0.0;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t total_insert_count = 0;
  std::size_t inserted_count = 0;
  double inserted_percent = 0.0;
  std::size_t transaction_count = 0;
  std::size_t query_transactions = 0;
  std::size_t insert_transactions = 0;
  long long build_ns = 0;
  long long elapsed_workload_ns = 0;
  long long maintenance_ns = 0;
  std::size_t insert_success_count = 0;
  std::size_t insert_failed_count = 0;
  std::size_t pieces = 0;
  std::size_t merge_count = 0;
  std::size_t delta_pending = 0;
  long long candidates_total = 0;
  long long answers_total = 0;
};

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --data_file DATA --query_file QUERY_1PCT.csv [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME          Dataset label in CSV (default: WKT)\n"
      << "  --limit N                    Number of valid WKT records (default: 10000)\n"
      << "  --workload read|write|both   90/10, 50/50, or both (default: both)\n"
      << "  --initial_fraction F         Bulk-loaded fraction (default: 0.5)\n"
      << "  --insert_fraction F          Fraction inserted before stop (default: 0.5)\n"
      << "  --insert_batch_percent F     Records per insert transaction (default: 0.01)\n"
      << "  --insert_order random|file|zmin  Remaining-record order (default: random)\n"
      << "  --piece_limit N              Records per piece (default: 10000)\n"
      << "  --cell_xmin X                Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y                Z-order latitude origin (default: -90)\n"
      << "  --cell_size S                Z-order cell size (default: 5e-7)\n"
      << "  --progress_step_percent N    Output checkpoint step (default: 10)\n"
      << "  --include_lsm_async_glin 0|1 Also run GLIN-LSM-async (default: 0)\n"
      << "  --delta_size N               Delta size before merge/rebuild (default: 100000)\n"
      << "  --seed N                     Random seed (default: 42)\n"
      << "  --output_csv PATH            Write summary CSV\n"
      << "  --progress_csv PATH          Write curve CSV\n"
      << "  --append_csv                 Append CSV instead of overwrite\n";
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
    } else if (key == "--query_file") {
      options.query_file = require_value(key);
    } else if (key == "--dataset_name") {
      options.dataset_name = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--workload") {
      options.workload = require_value(key);
    } else if (key == "--initial_fraction") {
      options.initial_fraction = std::stod(require_value(key));
    } else if (key == "--insert_fraction") {
      options.insert_fraction = std::stod(require_value(key));
    } else if (key == "--insert_batch_percent") {
      options.insert_batch_percent = std::stod(require_value(key));
    } else if (key == "--insert_order") {
      options.insert_order = require_value(key);
    } else if (key == "--piece_limit") {
      options.piece_limit = std::stod(require_value(key));
    } else if (key == "--cell_xmin") {
      options.cell_xmin = std::stod(require_value(key));
    } else if (key == "--cell_ymin") {
      options.cell_ymin = std::stod(require_value(key));
    } else if (key == "--cell_size") {
      options.cell_size = std::stod(require_value(key));
    } else if (key == "--progress_step_percent") {
      options.progress_step_percent =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--include_lsm_async_glin") {
      options.include_lsm_async_glin = std::stoi(require_value(key)) != 0;
    } else if (key == "--delta_size") {
      options.delta_size =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--output_csv") {
      options.output_csv = require_value(key);
    } else if (key == "--progress_csv") {
      options.progress_csv = require_value(key);
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
  if (options.query_file.empty()) {
    throw std::runtime_error("--query_file is required");
  }
  if (options.limit == 0) {
    throw std::runtime_error("--limit must be greater than 0");
  }
  if (options.workload != "read" && options.workload != "write" &&
      options.workload != "both") {
    throw std::runtime_error("--workload must be read, write, or both");
  }
  if (options.insert_order != "random" && options.insert_order != "file" &&
      options.insert_order != "zmin") {
    throw std::runtime_error("--insert_order must be random, file, or zmin");
  }
  if (options.initial_fraction <= 0.0 || options.initial_fraction >= 1.0) {
    throw std::runtime_error("--initial_fraction must be between 0 and 1");
  }
  if (options.insert_fraction <= 0.0 || options.insert_fraction > 1.0) {
    throw std::runtime_error("--insert_fraction must be between 0 and 1");
  }
  if (options.insert_batch_percent <= 0.0 ||
      options.insert_batch_percent > options.insert_fraction) {
    throw std::runtime_error(
        "--insert_batch_percent must be positive and no larger than --insert_fraction");
  }
  if (options.progress_step_percent == 0 ||
      options.progress_step_percent > 100) {
    throw std::runtime_error("--progress_step_percent must be between 1 and 100");
  }
  if (options.delta_size == 0) {
    throw std::runtime_error("--delta_size must be greater than 0");
  }
  return options;
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
  return "";
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

  auto start = std::chrono::high_resolution_clock::now();
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
  auto end = std::chrono::high_resolution_clock::now();
  stats.load_ns = ns_count(end - start);

  if (first_error_line != 0) {
    std::cerr << "First parse error at line " << first_error_line << ": "
              << first_error_message << "\n";
  }
  return geometries;
}

GeometryPtr make_rectangle(geos::geom::GeometryFactory& factory,
                           double xmin, double ymin, double xmax, double ymax) {
  geos::geom::CoordinateArraySequence coords;
  coords.add(geos::geom::Coordinate(xmin, ymin));
  coords.add(geos::geom::Coordinate(xmax, ymin));
  coords.add(geos::geom::Coordinate(xmax, ymax));
  coords.add(geos::geom::Coordinate(xmin, ymax));
  coords.add(geos::geom::Coordinate(xmin, ymin));
  std::unique_ptr<geos::geom::LinearRing> shell(factory.createLinearRing(coords));
  std::unique_ptr<geos::geom::Polygon> polygon =
      factory.createPolygon(std::move(shell));
  return GeometryPtr(polygon.release());
}

std::vector<QueryCase> load_query_file(const std::string& query_file,
                                       geos::geom::GeometryFactory& factory,
                                       std::vector<GeometryPtr>& owned_queries) {
  std::ifstream input(query_file);
  if (!input) {
    throw std::runtime_error("Cannot open query file: " + query_file);
  }

  std::vector<QueryCase> queries;
  std::string line;
  while (std::getline(input, line)) {
    strip_cr(line);
    if (line.empty()) {
      continue;
    }
    if (line.find("query_id") != std::string::npos) {
      continue;
    }

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) {
      cols.push_back(col);
    }
    if (cols.size() < 5) {
      continue;
    }

    QueryCase query;
    query.query_id = static_cast<std::size_t>(std::stoull(cols[0]));
    query.xmin = std::stod(cols[1]);
    query.ymin = std::stod(cols[2]);
    query.xmax = std::stod(cols[3]);
    query.ymax = std::stod(cols[4]);
    if (cols.size() > 5 && !cols[5].empty()) {
      query.source_geometry_id =
          static_cast<std::size_t>(std::stoull(cols[5]));
    }
    query.envelope =
        geos::geom::Envelope(query.xmin, query.xmax, query.ymin, query.ymax);
    GeometryPtr geometry =
        make_rectangle(factory, query.xmin, query.ymin, query.xmax, query.ymax);
    query.geometry = geometry.get();
    owned_queries.push_back(std::move(geometry));
    queries.push_back(query);
  }

  if (queries.empty()) {
    throw std::runtime_error("No query rows loaded from: " + query_file);
  }
  return queries;
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

std::vector<std::size_t> zmin_sorted_batch_ids(
    const Options& options, const std::vector<GeometryPtr>& geometries,
    const std::vector<std::size_t>& ids, std::size_t begin, std::size_t end) {
  std::vector<std::pair<double, std::size_t>> keyed_ids;
  keyed_ids.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    std::size_t id = ids[i];
    double zmin = 0.0;
    double zmax = 0.0;
    curve_shape_projection(geometries[id].get(), "z", options.cell_xmin,
                           options.cell_ymin, options.cell_size,
                           options.cell_size, zmin, zmax);
    keyed_ids.emplace_back(zmin, id);
  }
  std::sort(keyed_ids.begin(), keyed_ids.end(),
            [](const std::pair<double, std::size_t>& a,
               const std::pair<double, std::size_t>& b) {
              if (a.first != b.first) {
                return a.first < b.first;
              }
              return a.second < b.second;
            });

  std::vector<std::size_t> sorted_ids;
  sorted_ids.reserve(keyed_ids.size());
  for (const auto& keyed_id : keyed_ids) {
    sorted_ids.push_back(keyed_id.second);
  }
  return sorted_ids;
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

std::unique_ptr<alex::Glin<double, Geometry*>> build_glin_index_by_ids(
    const Options& options, const std::vector<GeometryPtr>& geometries,
    const std::vector<std::size_t>& ids,
    std::vector<std::tuple<double, double, double, double>>& pieces) {
  std::unique_ptr<alex::Glin<double, Geometry*>> index(
      new alex::Glin<double, Geometry*>());
  pieces.clear();
  std::vector<Geometry*> raw = raw_ptrs_by_ids(geometries, ids, 0, ids.size());
  index->glin_bulk_load(raw, options.piece_limit, "z", options.cell_xmin,
                        options.cell_ymin, options.cell_size,
                        options.cell_size, pieces);
  return index;
}

Box box_from_envelope(const geos::geom::Envelope* envelope) {
  return Box(Point(envelope->getMinX(), envelope->getMinY()),
             Point(envelope->getMaxX(), envelope->getMaxY()));
}

Box box_from_query(const QueryCase& query) {
  return Box(Point(query.xmin, query.ymin), Point(query.xmax, query.ymax));
}

std::vector<std::size_t> make_progress_checkpoints(std::size_t loaded_count,
                                                   std::size_t insert_count,
                                                   std::size_t step_percent) {
  std::vector<std::size_t> checkpoints;
  for (std::size_t percent = step_percent; percent <= 100;
       percent += step_percent) {
    std::size_t checkpoint = (loaded_count * percent + 99) / 100;
    if (checkpoint == 0 || checkpoint > insert_count) {
      continue;
    }
    if (checkpoints.empty() || checkpoints.back() != checkpoint) {
      checkpoints.push_back(checkpoint);
    }
  }
  if (checkpoints.empty() || checkpoints.back() != insert_count) {
    checkpoints.push_back(insert_count);
  }
  return checkpoints;
}

std::size_t query_transactions_before_insert(double query_fraction) {
  double insert_fraction = 1.0 - query_fraction;
  if (insert_fraction <= 0.0) {
    throw std::runtime_error("query_fraction must be smaller than 1");
  }
  return static_cast<std::size_t>(
      std::max(0.0, std::round(query_fraction / insert_fraction)));
}

void maybe_capture_progress(std::vector<ProgressResult>& progress,
                            const std::string& index_name,
                            const std::string& workload,
                            double query_fraction,
                            std::size_t loaded_count,
                            std::size_t initial_count,
                            std::size_t total_insert_count,
                            const std::vector<std::size_t>& checkpoints,
                            std::size_t& next_checkpoint,
                            std::chrono::high_resolution_clock::time_point start,
                            const HybridResult& state) {
  while (next_checkpoint < checkpoints.size() &&
         state.insert_success_count >= checkpoints[next_checkpoint]) {
    auto now = std::chrono::high_resolution_clock::now();
    ProgressResult row;
    row.index = index_name;
    row.workload = workload;
    row.query_fraction = query_fraction;
    row.loaded_count = loaded_count;
    row.initial_count = initial_count;
    row.total_insert_count = total_insert_count;
    row.inserted_count = checkpoints[next_checkpoint];
    row.inserted_percent =
        100.0 * static_cast<double>(row.inserted_count) /
        static_cast<double>(loaded_count);
    row.transaction_count = state.transaction_count;
    row.query_transactions = state.query_transactions;
    row.insert_transactions = state.insert_transactions;
    row.build_ns = state.build_ns;
    row.elapsed_workload_ns = ns_count(now - start);
    row.maintenance_ns = state.maintenance_ns;
    row.insert_success_count = state.insert_success_count;
    row.insert_failed_count = state.insert_failed_count;
    row.pieces = state.pieces;
    row.merge_count = state.merge_count;
    row.delta_pending = state.delta_pending;
    row.candidates_total = state.candidates_total;
    row.answers_total = state.answers_total;
    progress.push_back(row);
    ++next_checkpoint;
  }
}

HybridResult run_glin_hybrid(const Options& options,
                             const std::vector<GeometryPtr>& geometries,
                             const std::vector<std::size_t>& ids,
                             const std::vector<QueryCase>& queries,
                             const std::string& workload,
                             double query_fraction,
                             std::size_t initial_count,
                             std::size_t total_insert_count,
                             std::size_t batch_size,
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

  HybridResult result;
  result.index = "GLIN_PIECEWISE";
  result.workload = workload;
  result.query_fraction = query_fraction;
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.total_insert_count = total_insert_count;
  result.build_ns = ns_count(build_end - build_start);
  result.pieces = pieces.size();

  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), total_insert_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;
  std::size_t next_query = 0;
  std::size_t next_insert_offset = 0;
  std::size_t queries_per_insert =
      query_transactions_before_insert(query_fraction);

  auto workload_start = std::chrono::high_resolution_clock::now();
  while (next_insert_offset < total_insert_count) {
    for (std::size_t q = 0; q < queries_per_insert; ++q) {
      const QueryCase& query = queries[next_query % queries.size()];
      ++next_query;
      int count_filter = 0;
      std::vector<Geometry*> find_result;
      index.glin_find(query.geometry, "z", options.cell_xmin, options.cell_ymin,
                      options.cell_size, options.cell_size, pieces, find_result,
                      count_filter);
      ++result.query_transactions;
      ++result.transaction_count;
      result.candidates_total += count_filter;
      result.answers_total += static_cast<long long>(find_result.size());
    }

    std::size_t remaining = total_insert_count - next_insert_offset;
    std::size_t current_batch = std::min(batch_size, remaining);
    for (std::size_t i = 0; i < current_batch; ++i) {
      std::size_t id = ids[initial_count + next_insert_offset + i];
      Geometry* geometry = geometries[id].get();
      geos::geom::Envelope* envelope =
          const_cast<geos::geom::Envelope*>(geometry->getEnvelopeInternal());
      auto inserted = index.glin_insert(
          std::make_tuple(geometry, envelope), "z", options.cell_xmin,
          options.cell_ymin, options.cell_size, options.cell_size,
          options.piece_limit, pieces);
      if (inserted.second) {
        ++result.insert_success_count;
      } else {
        ++result.insert_failed_count;
      }
    }
    next_insert_offset += current_batch;
    ++result.insert_transactions;
    ++result.transaction_count;
    result.pieces = pieces.size();
    maybe_capture_progress(progress, result.index, workload, query_fraction,
                           geometries.size(), initial_count, total_insert_count,
                           checkpoints, next_checkpoint, workload_start,
                           result);
  }
  auto workload_end = std::chrono::high_resolution_clock::now();
  result.workload_ns = ns_count(workload_end - workload_start);
  return result;
}

HybridResult run_glin_lsm_async_hybrid(
    const Options& options, const std::vector<GeometryPtr>& geometries,
    const std::vector<std::size_t>& ids, const std::vector<QueryCase>& queries,
    const std::string& workload, double query_fraction,
    std::size_t initial_count, std::size_t total_insert_count,
    std::size_t batch_size, std::vector<ProgressResult>& progress) {
  std::vector<std::size_t> main_ids(ids.begin(), ids.begin() + initial_count);
  main_ids = zmin_sorted_batch_ids(options, geometries, main_ids, 0,
                                   main_ids.size());

  std::vector<std::tuple<double, double, double, double>> pieces;
  auto build_start = std::chrono::high_resolution_clock::now();
  std::unique_ptr<alex::Glin<double, Geometry*>> main_index =
      build_glin_index_by_ids(options, geometries, main_ids, pieces);
  auto build_end = std::chrono::high_resolution_clock::now();

  RTree delta_index;
  std::vector<std::size_t> delta_ids;
  delta_ids.reserve(options.delta_size);

  HybridResult result;
  result.index = "GLIN_LSM_ASYNC";
  result.workload = workload;
  result.query_fraction = query_fraction;
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.total_insert_count = total_insert_count;
  result.build_ns = ns_count(build_end - build_start);
  result.pieces = pieces.size();

  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), total_insert_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;
  std::size_t next_query = 0;
  std::size_t next_insert_offset = 0;
  std::size_t queries_per_insert =
      query_transactions_before_insert(query_fraction);

  auto workload_start = std::chrono::high_resolution_clock::now();
  while (next_insert_offset < total_insert_count) {
    for (std::size_t q = 0; q < queries_per_insert; ++q) {
      const QueryCase& query = queries[next_query % queries.size()];
      ++next_query;

      int main_candidates = 0;
      std::vector<Geometry*> main_result;
      main_index->glin_find(query.geometry, "z", options.cell_xmin,
                            options.cell_ymin, options.cell_size,
                            options.cell_size, pieces, main_result,
                            main_candidates);

      std::vector<RTreeValue> delta_result;
      delta_index.query(bgi::intersects(box_from_query(query)),
                        std::back_inserter(delta_result));
      std::size_t delta_answers = 0;
      for (const auto& candidate : delta_result) {
        Geometry* payload = geometries[candidate.second].get();
        if (query.geometry->intersects(payload)) {
          ++delta_answers;
        }
      }

      ++result.query_transactions;
      ++result.transaction_count;
      result.candidates_total +=
          static_cast<long long>(main_candidates + delta_result.size());
      result.answers_total +=
          static_cast<long long>(main_result.size() + delta_answers);
    }

    std::size_t remaining = total_insert_count - next_insert_offset;
    std::size_t current_batch = std::min(batch_size, remaining);
    for (std::size_t i = 0; i < current_batch; ++i) {
      std::size_t id = ids[initial_count + next_insert_offset + i];
      delta_index.insert(std::make_pair(
          box_from_envelope(geometries[id]->getEnvelopeInternal()), id));
      delta_ids.push_back(id);
      ++result.insert_success_count;

      if (delta_ids.size() >= options.delta_size) {
        auto maintenance_start = std::chrono::high_resolution_clock::now();
        main_ids.insert(main_ids.end(), delta_ids.begin(), delta_ids.end());
        main_ids = zmin_sorted_batch_ids(options, geometries, main_ids, 0,
                                         main_ids.size());
        main_index = build_glin_index_by_ids(options, geometries, main_ids,
                                             pieces);
        delta_index.clear();
        delta_ids.clear();
        auto maintenance_end = std::chrono::high_resolution_clock::now();
        result.maintenance_ns +=
            ns_count(maintenance_end - maintenance_start);
        ++result.merge_count;
      }
    }
    next_insert_offset += current_batch;
    ++result.insert_transactions;
    ++result.transaction_count;
    result.pieces = pieces.size();
    result.delta_pending = delta_ids.size();
    maybe_capture_progress(progress, result.index, workload, query_fraction,
                           geometries.size(), initial_count, total_insert_count,
                           checkpoints, next_checkpoint, workload_start,
                           result);
  }
  auto workload_end = std::chrono::high_resolution_clock::now();
  result.workload_ns = ns_count(workload_end - workload_start);
  result.delta_pending = delta_ids.size();
  return result;
}

template <typename QueryFn, typename InsertFn>
HybridResult run_tree_hybrid(const Options& options,
                             const std::string& index_name,
                             const std::vector<GeometryPtr>& geometries,
                             const std::vector<std::size_t>& ids,
                             const std::vector<QueryCase>& queries,
                             const std::string& workload,
                             double query_fraction,
                             std::size_t initial_count,
                             std::size_t total_insert_count,
                             std::size_t batch_size,
                             long long build_ns,
                             QueryFn query_fn,
                             InsertFn insert_fn,
                             std::vector<ProgressResult>& progress) {
  HybridResult result;
  result.index = index_name;
  result.workload = workload;
  result.query_fraction = query_fraction;
  result.loaded_count = geometries.size();
  result.initial_count = initial_count;
  result.total_insert_count = total_insert_count;
  result.build_ns = build_ns;

  std::vector<std::size_t> checkpoints = make_progress_checkpoints(
      geometries.size(), total_insert_count, options.progress_step_percent);
  std::size_t next_checkpoint = 0;
  std::size_t next_query = 0;
  std::size_t next_insert_offset = 0;
  std::size_t queries_per_insert =
      query_transactions_before_insert(query_fraction);

  auto workload_start = std::chrono::high_resolution_clock::now();
  while (next_insert_offset < total_insert_count) {
    for (std::size_t q = 0; q < queries_per_insert; ++q) {
      const QueryCase& query = queries[next_query % queries.size()];
      ++next_query;
      std::size_t candidates = 0;
      std::size_t answers = 0;
      query_fn(query, candidates, answers);
      ++result.query_transactions;
      ++result.transaction_count;
      result.candidates_total += static_cast<long long>(candidates);
      result.answers_total += static_cast<long long>(answers);
    }

    std::size_t remaining = total_insert_count - next_insert_offset;
    std::size_t current_batch = std::min(batch_size, remaining);
    for (std::size_t i = 0; i < current_batch; ++i) {
      std::size_t id = ids[initial_count + next_insert_offset + i];
      if (insert_fn(id)) {
        ++result.insert_success_count;
      } else {
        ++result.insert_failed_count;
      }
    }
    next_insert_offset += current_batch;
    ++result.insert_transactions;
    ++result.transaction_count;
    maybe_capture_progress(progress, result.index, workload, query_fraction,
                           geometries.size(), initial_count, total_insert_count,
                           checkpoints, next_checkpoint, workload_start,
                           result);
  }
  auto workload_end = std::chrono::high_resolution_clock::now();
  result.workload_ns = ns_count(workload_end - workload_start);
  return result;
}

HybridResult run_boost_hybrid(const Options& options,
                              const std::vector<GeometryPtr>& geometries,
                              const std::vector<std::size_t>& ids,
                              const std::vector<QueryCase>& queries,
                              const std::string& workload,
                              double query_fraction,
                              std::size_t initial_count,
                              std::size_t total_insert_count,
                              std::size_t batch_size,
                              std::vector<ProgressResult>& progress) {
  std::vector<RTreeValue> values;
  values.reserve(initial_count);
  for (std::size_t i = 0; i < initial_count; ++i) {
    std::size_t id = ids[i];
    values.emplace_back(box_from_envelope(geometries[id]->getEnvelopeInternal()), id);
  }

  auto build_start = std::chrono::high_resolution_clock::now();
  RTree rtree(values.begin(), values.end());
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);

  auto query_fn = [&](const QueryCase& query, std::size_t& candidates,
                      std::size_t& answers) {
    std::vector<RTreeValue> result;
    rtree.query(bgi::intersects(box_from_query(query)), std::back_inserter(result));
    candidates = result.size();
    answers = 0;
    for (const auto& candidate : result) {
      Geometry* payload = geometries[candidate.second].get();
      if (query.geometry->intersects(payload)) {
        ++answers;
      }
    }
  };

  auto insert_fn = [&](std::size_t id) {
    rtree.insert(std::make_pair(
        box_from_envelope(geometries[id]->getEnvelopeInternal()), id));
    return true;
  };

  return run_tree_hybrid(options, "Boost_Rtree", geometries, ids, queries,
                         workload, query_fraction, initial_count,
                         total_insert_count, batch_size, build_ns, query_fn,
                         insert_fn, progress);
}

HybridResult run_quadtree_hybrid(const Options& options,
                                 const std::vector<GeometryPtr>& geometries,
                                 const std::vector<std::size_t>& ids,
                                 const std::vector<QueryCase>& queries,
                                 const std::string& workload,
                                 double query_fraction,
                                 std::size_t initial_count,
                                 std::size_t total_insert_count,
                                 std::size_t batch_size,
                                 std::vector<ProgressResult>& progress) {
  Quadtree quadtree;
  auto build_start = std::chrono::high_resolution_clock::now();
  for (std::size_t i = 0; i < initial_count; ++i) {
    Geometry* geometry = geometries[ids[i]].get();
    quadtree.insert(geometry->getEnvelopeInternal(), geometry);
  }
  auto build_end = std::chrono::high_resolution_clock::now();
  long long build_ns = ns_count(build_end - build_start);

  auto query_fn = [&](const QueryCase& query, std::size_t& candidates,
                      std::size_t& answers) {
    std::vector<void*> result;
    quadtree.query(&query.envelope, result);
    candidates = result.size();
    answers = 0;
    for (void* candidate : result) {
      Geometry* payload = static_cast<Geometry*>(candidate);
      if (query.geometry->intersects(payload)) {
        ++answers;
      }
    }
  };

  auto insert_fn = [&](std::size_t id) {
    Geometry* geometry = geometries[id].get();
    quadtree.insert(geometry->getEnvelopeInternal(), geometry);
    return true;
  };

  return run_tree_hybrid(options, "GEOS_Quadtree", geometries, ids, queries,
                         workload, query_fraction, initial_count,
                         total_insert_count, batch_size, build_ns, query_fn,
                         insert_fn, progress);
}

bool output_has_content(const std::string& path) {
  std::ifstream input(path);
  return input.good() && input.peek() != std::ifstream::traits_type::eof();
}

std::ios_base::openmode output_mode(bool append) {
  std::ios_base::openmode mode = std::ios::out;
  if (append) {
    mode |= std::ios::app;
  }
  return mode;
}

void write_summary_csv(const std::string& path, const Options& options,
                       const LoadStats& stats,
                       const std::vector<HybridResult>& results) {
  if (path.empty()) {
    return;
  }
  bool write_header = !(options.append_csv && output_has_content(path));
  std::ofstream output(path, output_mode(options.append_csv));
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }
  if (write_header) {
    output << "dataset,index,relationship,workload,query_fraction,"
              "insert_batch_percent,loaded_count,initial_count,total_insert_count,"
              "insert_order,piece_limit,cell_xmin,cell_ymin,cell_size,seed,"
              "lines_seen,parse_errors,load_ns,build_ns,workload_ns,maintenance_ns,"
              "throughput_transactions_per_sec,transaction_count,query_transactions,"
              "insert_transactions,insert_success_count,insert_failed_count,pieces,"
              "merge_count,delta_pending,delta_size,candidates_total,answers_total\n";
  }
  for (const auto& result : results) {
    double throughput = result.workload_ns == 0
                            ? 0.0
                            : static_cast<double>(result.transaction_count) * 1e9 /
                                  static_cast<double>(result.workload_ns);
    output << options.dataset_name << "," << result.index << ",intersects,"
           << result.workload << "," << result.query_fraction << ","
           << options.insert_batch_percent << "," << result.loaded_count << ","
           << result.initial_count << "," << result.total_insert_count << ","
           << options.insert_order << "," << options.piece_limit << ","
           << options.cell_xmin << "," << options.cell_ymin << ","
           << options.cell_size << "," << options.seed << ","
           << stats.lines_seen << "," << stats.parse_errors << ","
           << stats.load_ns << "," << result.build_ns << ","
           << result.workload_ns << "," << result.maintenance_ns << ","
           << throughput << ","
           << result.transaction_count << "," << result.query_transactions << ","
           << result.insert_transactions << "," << result.insert_success_count
           << "," << result.insert_failed_count << "," << result.pieces << ","
           << result.merge_count << "," << result.delta_pending << ","
           << options.delta_size << ","
           << result.candidates_total << "," << result.answers_total << "\n";
  }
}

void write_progress_csv(const std::string& path, const Options& options,
                        const LoadStats& stats,
                        const std::vector<ProgressResult>& results) {
  if (path.empty()) {
    return;
  }
  bool write_header = !(options.append_csv && output_has_content(path));
  std::ofstream output(path, output_mode(options.append_csv));
  if (!output) {
    throw std::runtime_error("Cannot open progress CSV: " + path);
  }
  if (write_header) {
    output << "dataset,index,relationship,workload,query_fraction,"
              "insert_batch_percent,loaded_count,initial_count,total_insert_count,"
              "inserted_count,inserted_percent,transaction_count,query_transactions,"
              "insert_transactions,insert_order,piece_limit,cell_xmin,cell_ymin,"
              "cell_size,seed,lines_seen,parse_errors,load_ns,build_ns,"
              "elapsed_workload_ns,maintenance_ns,throughput_transactions_per_sec,"
              "insert_success_count,insert_failed_count,pieces,merge_count,"
              "delta_pending,delta_size,candidates_total,answers_total\n";
  }
  for (const auto& result : results) {
    double throughput =
        result.elapsed_workload_ns == 0
            ? 0.0
            : static_cast<double>(result.transaction_count) * 1e9 /
                  static_cast<double>(result.elapsed_workload_ns);
    output << options.dataset_name << "," << result.index << ",intersects,"
           << result.workload << "," << result.query_fraction << ","
           << options.insert_batch_percent << "," << result.loaded_count << ","
           << result.initial_count << "," << result.total_insert_count << ","
           << result.inserted_count << "," << result.inserted_percent << ","
           << result.transaction_count << "," << result.query_transactions << ","
           << result.insert_transactions << "," << options.insert_order << ","
           << options.piece_limit << "," << options.cell_xmin << ","
           << options.cell_ymin << "," << options.cell_size << ","
           << options.seed << "," << stats.lines_seen << ","
           << stats.parse_errors << "," << stats.load_ns << ","
           << result.build_ns << "," << result.elapsed_workload_ns << ","
           << result.maintenance_ns << "," << throughput << ","
           << result.insert_success_count << ","
           << result.insert_failed_count << "," << result.pieces << ","
           << result.merge_count << "," << result.delta_pending << ","
           << options.delta_size << ","
           << result.candidates_total << "," << result.answers_total << "\n";
  }
}

std::vector<std::pair<std::string, double>> selected_workloads(
    const Options& options) {
  if (options.workload == "read") {
    return {{"read_intensive", 0.9}};
  }
  if (options.workload == "write") {
    return {{"write_intensive", 0.5}};
  }
  return {{"read_intensive", 0.9}, {"write_intensive", 0.5}};
}

void print_result(const HybridResult& result) {
  double throughput = result.workload_ns == 0
                          ? 0.0
                          : static_cast<double>(result.transaction_count) * 1e9 /
                                static_cast<double>(result.workload_ns);
  std::cout << std::fixed << std::setprecision(3)
            << "loaded=" << result.loaded_count
            << " index=" << result.index
            << " workload=" << result.workload
            << " tx=" << result.transaction_count
            << " query_tx=" << result.query_transactions
            << " insert_tx=" << result.insert_transactions
            << " inserted=" << result.insert_success_count
            << " failed=" << result.insert_failed_count
            << " pieces=" << result.pieces
            << " merges=" << result.merge_count
            << " delta_pending=" << result.delta_pending
            << " build_ms=" << result.build_ns / 1e6
            << " workload_ms=" << result.workload_ns / 1e6
            << " maintenance_ms=" << result.maintenance_ns / 1e6
            << " throughput_tx_per_sec=" << throughput << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    LoadStats stats;
    std::vector<GeometryPtr> geometries = load_wkt_csv(options, reader, stats);
    if (geometries.size() < 2) {
      throw std::runtime_error("Need at least two valid geometries");
    }

    std::vector<GeometryPtr> owned_queries;
    std::vector<QueryCase> queries =
        load_query_file(options.query_file, *factory, owned_queries);
    std::vector<std::size_t> ids = make_ordered_ids(options, geometries);

    std::size_t initial_count = static_cast<std::size_t>(
        static_cast<double>(geometries.size()) * options.initial_fraction);
    initial_count =
        std::max<std::size_t>(1, std::min(initial_count, geometries.size() - 1));
    std::size_t total_insert_count = static_cast<std::size_t>(
        static_cast<double>(geometries.size()) * options.insert_fraction);
    total_insert_count = std::max<std::size_t>(
        1, std::min(total_insert_count, geometries.size() - initial_count));
    std::size_t batch_size = static_cast<std::size_t>(
        static_cast<double>(geometries.size()) * options.insert_batch_percent);
    batch_size = std::max<std::size_t>(1, std::min(batch_size, total_insert_count));

    std::vector<HybridResult> summary_results;
    std::vector<ProgressResult> progress_results;

    for (const auto& workload : selected_workloads(options)) {
      summary_results.push_back(run_glin_hybrid(
          options, geometries, ids, queries, workload.first, workload.second,
          initial_count, total_insert_count, batch_size, progress_results));
      if (options.include_lsm_async_glin) {
        summary_results.push_back(run_glin_lsm_async_hybrid(
            options, geometries, ids, queries, workload.first, workload.second,
            initial_count, total_insert_count, batch_size, progress_results));
      }
      summary_results.push_back(run_boost_hybrid(
          options, geometries, ids, queries, workload.first, workload.second,
          initial_count, total_insert_count, batch_size, progress_results));
      summary_results.push_back(run_quadtree_hybrid(
          options, geometries, ids, queries, workload.first, workload.second,
          initial_count, total_insert_count, batch_size, progress_results));
    }

    write_summary_csv(options.output_csv, options, stats, summary_results);
    write_progress_csv(options.progress_csv, options, stats, progress_results);
    for (const auto& result : summary_results) {
      print_result(result);
    }
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }
  return 0;
}
