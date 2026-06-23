// Unified dynamic comparison for exact Intersects over WKT geometries.
//
// All indexes use the same bulk-load/insert/delete/query workload:
//   bulk-load initial_fraction, insert insert_fraction, query,
//   delete delete_fraction, query.
//
// This file intentionally keeps the DELI-Dynamic-Single prototype local so the
// old standalone benchmark remains untouched.

#include "../../glin/glin.h"

#include <geos/geom/Coordinate.h>
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/LinearRing.h>
#include <geos/index/quadtree/Quadtree.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
using ObjectId = std::uint64_t;
using RecordId = std::uint64_t;

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;
using BoostPoint = bg::model::d2::point_xy<double>;
using BoostBox = bg::model::box<BoostPoint>;
using RTreeValue = std::pair<BoostBox, ObjectId>;
using RTree = bgi::rtree<RTreeValue, bgi::quadratic<16>>;
using Quadtree = geos::index::quadtree::Quadtree;

struct Options {
  std::string data_file;
  std::string dataset_name = "WKT";
  std::string query_file;
  std::string output_csv;
  std::string workload_mode = "staged";
  std::string mixed_profile = "custom";
  std::string mixed_profile_specs;
  std::string indexes = "all";
  std::size_t limit = 10000;
  std::size_t query_count = 100;
  double initial_fraction = 0.5;
  double insert_fraction = 0.2;
  double delete_fraction = 0.1;
  std::size_t mixed_operations = 100000;
  std::size_t mixed_checkpoint_interval = 10000;
  double mixed_query_ratio = 0.90;
  double mixed_insert_ratio = 0.05;
  double mixed_delete_ratio = 0.05;
  std::size_t block_size = 512;
  double stale_threshold_fraction = 0.05;
  std::size_t local_delta_bound = 0;
  double delete_compact_fraction = 0.25;
  double piece_limit = 10000.0;
  std::uint64_t seed = 42;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  std::size_t validate_every = 0;
  bool check_correctness = true;
  std::size_t correctness_every_n = 1;
  bool stop_on_mismatch = true;
};

struct LoadStats {
  std::size_t lines_seen = 0;
  std::size_t parse_errors = 0;
};

struct QueryCase {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  Geometry* geometry = nullptr;
};

struct DynamicBlock;

struct DynamicRecord {
  ObjectId object_id = 0;
  RecordId record_id = 0;
  double zmin = 0.0;
  double zmax = 0.0;
  geos::geom::Envelope envelope;
  Geometry* geometry = nullptr;
  bool alive = true;
  std::uint64_t version = 0;
  DynamicBlock* block = nullptr;
};

struct DynamicBlock {
  std::uint64_t block_id = 0;
  std::vector<RecordId> ids;
  double min_zmin = std::numeric_limits<double>::infinity();
  double max_zmin = -std::numeric_limits<double>::infinity();
  double max_zmax = -std::numeric_limits<double>::infinity();
  geos::geom::Envelope mbr;
  std::size_t live_count = 0;
  std::size_t dead_count = 0;
  bool stale = false;
};

struct QueryResult {
  long long query_ns = 0;
  std::size_t prefix_blocks = 0;
  std::size_t visited_blocks = 0;
  std::size_t skipped_zmax_blocks = 0;
  std::size_t skipped_mbr_blocks = 0;
  std::size_t records_scanned = 0;
  std::size_t interval_candidates = 0;
  std::size_t mbr_candidates = 0;
  std::size_t exact_calls = 0;
  std::unordered_set<ObjectId> answers;
};

struct GeometryMeta {
  ObjectId object_id = 0;
  double zmin = 0.0;
  double zmax = 0.0;
  geos::geom::Envelope envelope;
};

struct QueryAggregate {
  std::vector<long long> latencies_ns;
  std::size_t prefix_blocks = 0;
  std::size_t visited_blocks = 0;
  std::size_t skipped_zmax_blocks = 0;
  std::size_t skipped_mbr_blocks = 0;
  std::size_t records_scanned = 0;
  std::size_t interval_candidates = 0;
  std::size_t mbr_candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t answers = 0;
  std::size_t zero_answer_queries = 0;
};

struct CheckpointSummary {
  std::string checkpoint;
  std::size_t checkpoint_id = 0;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t insert_count = 0;
  std::size_t delete_count = 0;
  std::size_t live_count = 0;
  std::size_t total_records = 0;
  std::size_t dead_records = 0;
  double dead_entry_ratio = 0.0;
  std::size_t query_count = 0;
  std::size_t block_size = 0;
  std::size_t block_count = 0;
  double stale_threshold_fraction = 0.0;
  std::size_t stale_block_count = 0;
  std::size_t summary_rebuild_count = 0;
  long long summary_rebuild_ns = 0;
  std::size_t block_split_count = 0;
  long long avg_query_ns = 0;
  long long p50_query_ns = 0;
  long long p95_query_ns = 0;
  long long p99_query_ns = 0;
  std::size_t records_scanned = 0;
  std::size_t visited_blocks = 0;
  std::size_t skipped_zmax_blocks = 0;
  std::size_t skipped_mbr_blocks = 0;
  double skipped_block_ratio = 0.0;
  std::size_t interval_candidates = 0;
  std::size_t mbr_candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t answers = 0;
  double candidate_answer_ratio = 0.0;
  std::size_t zero_answer_queries = 0;
  int answers_match_boost = 1;
  std::size_t missing_count = 0;
  std::size_t extra_count = 0;
  long long boost_rebuild_ns = 0;
  long long boost_query_ns = 0;
  long long insert_ns = 0;
  long long delete_ns = 0;
  double insert_tps = 0.0;
  double delete_tps = 0.0;
  int validate_ok = 1;
};

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file data.wkt --query_file queries.csv [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME              Dataset label (default: WKT)\n"
      << "  --limit N                        Valid geometries to load (default: 10000)\n"
      << "  --query_file PATH                Query CSV file\n"
      << "  --query_count N                  Max queries to run (default: 100)\n"
      << "  --workload_mode staged|mixed     Staged checkpoints or interleaved mixed workload (default: staged)\n"
      << "  --mixed_profile NAME             Mixed workload label in output CSV (default: custom)\n"
      << "  --mixed_profile_specs LIST       Batched mixed profiles as name:q:i:d entries\n"
      << "  --mixed_operations N             Number of interleaved operations after bulk-load (default: 100000)\n"
      << "  --mixed_checkpoint_interval N    Emit a mixed checkpoint every N operations (default: 10000)\n"
      << "  --mixed_query_ratio F            Mixed query probability (default: 0.90)\n"
      << "  --mixed_insert_ratio F           Mixed insert probability (default: 0.05)\n"
      << "  --mixed_delete_ratio F           Mixed delete probability (default: 0.05)\n"
      << "  --indexes LIST                   Space/comma separated indexes to run, or all (default: all)\n"
      << "  --initial_fraction F             Bulk-load fraction (default: 0.5)\n"
      << "  --insert_fraction F              Insert fraction of loaded data (default: 0.2)\n"
      << "  --delete_fraction F              Delete fraction of loaded data (default: 0.1)\n"
      << "  --block_size N                   Target live records per block (default: 512)\n"
      << "  --stale_threshold_fraction F     Dead-count rebuild threshold as block fraction (default: 0.05)\n"
      << "  --local_delta_bound N            Max per-block delta records before local compaction; 0 uses max(64, block_size/4)\n"
      << "  --delete_compact_fraction F      LocalBounded tombstone fraction before physical compaction (default: 0.25)\n"
      << "  --piece_limit N                  GLIN-piece records per piece (default: 10000)\n"
      << "  --seed N                         Random seed (default: 42)\n"
      << "  --cell_xmin X                    Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y                    Z-order latitude origin (default: -90)\n"
      << "  --cell_size S                    Z-order cell size (default: 5e-7)\n"
      << "  --validate_every N               Validate every N updates; 0 disables (default: 0)\n"
      << "  --check_correctness 0|1          Compare each checkpoint with rebuilt Boost R-tree (default: 1)\n"
      << "  --correctness_every_n N          Check every N-th checkpoint; 0 disables oracle checks (default: 1)\n"
      << "  --stop_on_mismatch 0|1           Stop if Boost answers differ (default: 1)\n"
      << "  --output_csv PATH                Write checkpoint summary CSV\n";
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
    } else if (key == "--query_file") {
      options.query_file = require_value(key);
    } else if (key == "--output_csv") {
      options.output_csv = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--query_count") {
      options.query_count =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--workload_mode") {
      options.workload_mode = require_value(key);
    } else if (key == "--mixed_profile") {
      options.mixed_profile = require_value(key);
    } else if (key == "--mixed_profile_specs") {
      options.mixed_profile_specs = require_value(key);
    } else if (key == "--mixed_operations") {
      options.mixed_operations =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--mixed_checkpoint_interval") {
      options.mixed_checkpoint_interval =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--mixed_query_ratio") {
      options.mixed_query_ratio = std::stod(require_value(key));
    } else if (key == "--mixed_insert_ratio") {
      options.mixed_insert_ratio = std::stod(require_value(key));
    } else if (key == "--mixed_delete_ratio") {
      options.mixed_delete_ratio = std::stod(require_value(key));
    } else if (key == "--indexes") {
      options.indexes = require_value(key);
    } else if (key == "--initial_fraction") {
      options.initial_fraction = std::stod(require_value(key));
    } else if (key == "--insert_fraction") {
      options.insert_fraction = std::stod(require_value(key));
    } else if (key == "--delete_fraction") {
      options.delete_fraction = std::stod(require_value(key));
    } else if (key == "--block_size") {
      options.block_size =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--stale_threshold_fraction") {
      options.stale_threshold_fraction = std::stod(require_value(key));
    } else if (key == "--local_delta_bound") {
      options.local_delta_bound =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--delete_compact_fraction") {
      options.delete_compact_fraction = std::stod(require_value(key));
    } else if (key == "--piece_limit") {
      options.piece_limit = std::stod(require_value(key));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--cell_xmin") {
      options.cell_xmin = std::stod(require_value(key));
    } else if (key == "--cell_ymin") {
      options.cell_ymin = std::stod(require_value(key));
    } else if (key == "--cell_size") {
      options.cell_size = std::stod(require_value(key));
    } else if (key == "--validate_every") {
      options.validate_every =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--check_correctness") {
      options.check_correctness = std::stoi(require_value(key)) != 0;
    } else if (key == "--correctness_every_n") {
      options.correctness_every_n =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--stop_on_mismatch") {
      options.stop_on_mismatch = std::stoi(require_value(key)) != 0;
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
  if (options.query_count == 0) {
    throw std::runtime_error("--query_count must be greater than 0");
  }
  if (options.block_size == 0) {
    throw std::runtime_error("--block_size must be greater than 0");
  }
  auto validate_fraction = [](double value, const char* name) {
    if (value < 0.0 || value > 1.0) {
      throw std::runtime_error(std::string(name) + " must be in [0, 1]");
    }
  };
  validate_fraction(options.initial_fraction, "--initial_fraction");
  validate_fraction(options.insert_fraction, "--insert_fraction");
  validate_fraction(options.delete_fraction, "--delete_fraction");
  validate_fraction(options.mixed_query_ratio, "--mixed_query_ratio");
  validate_fraction(options.mixed_insert_ratio, "--mixed_insert_ratio");
  validate_fraction(options.mixed_delete_ratio, "--mixed_delete_ratio");
  validate_fraction(options.delete_compact_fraction,
                    "--delete_compact_fraction");
  if (options.initial_fraction <= 0.0) {
    throw std::runtime_error("--initial_fraction must be greater than 0");
  }
  if (options.stale_threshold_fraction < 0.0) {
    throw std::runtime_error("--stale_threshold_fraction must be non-negative");
  }
  if (options.workload_mode != "staged" && options.workload_mode != "mixed") {
    throw std::runtime_error("--workload_mode must be staged or mixed");
  }
  if (options.workload_mode == "mixed") {
    if (options.mixed_operations == 0) {
      throw std::runtime_error("--mixed_operations must be greater than 0");
    }
    if (options.mixed_checkpoint_interval == 0) {
      throw std::runtime_error(
          "--mixed_checkpoint_interval must be greater than 0");
    }
    const double ratio_sum = options.mixed_query_ratio +
                             options.mixed_insert_ratio +
                             options.mixed_delete_ratio;
    if (ratio_sum <= 0.0) {
      throw std::runtime_error("mixed workload ratios must sum to > 0");
    }
    options.mixed_query_ratio /= ratio_sum;
    options.mixed_insert_ratio /= ratio_sum;
    options.mixed_delete_ratio /= ratio_sum;
  }
  if (options.correctness_every_n == 0) {
    options.check_correctness = false;
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

  if (first_error_line != 0) {
    std::cerr << "First parse error at line " << first_error_line << ": "
              << first_error_message << "\n";
  }
  return geometries;
}

bool envelopes_intersect(const geos::geom::Envelope& a,
                         const geos::geom::Envelope& b) {
  return !(a.getMaxX() < b.getMinX() || b.getMaxX() < a.getMinX() ||
           a.getMaxY() < b.getMinY() || b.getMaxY() < a.getMinY());
}

bool envelope_contains(const geos::geom::Envelope& outer,
                       const geos::geom::Envelope& inner) {
  return outer.getMinX() <= inner.getMinX() &&
         outer.getMaxX() >= inner.getMaxX() &&
         outer.getMinY() <= inner.getMinY() &&
         outer.getMaxY() >= inner.getMaxY();
}

BoostBox boost_box_from_envelope(const geos::geom::Envelope& envelope) {
  return BoostBox(BoostPoint(envelope.getMinX(), envelope.getMinY()),
                  BoostPoint(envelope.getMaxX(), envelope.getMaxY()));
}

GeometryPtr make_query_box(geos::geom::GeometryFactory& factory,
                           double xmin, double ymin, double xmax,
                           double ymax) {
  if (xmin == xmax) {
    xmax += 1e-12;
  }
  if (ymin == ymax) {
    ymax += 1e-12;
  }

  auto* coordinates = new geos::geom::CoordinateArraySequence();
  coordinates->add(geos::geom::Coordinate(xmin, ymin));
  coordinates->add(geos::geom::Coordinate(xmin, ymax));
  coordinates->add(geos::geom::Coordinate(xmax, ymax));
  coordinates->add(geos::geom::Coordinate(xmax, ymin));
  coordinates->add(geos::geom::Coordinate(xmin, ymin));

  geos::geom::LinearRing* ring = factory.createLinearRing(coordinates);
  return GeometryPtr(factory.createPolygon(ring, NULL));
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream ss(line);
  std::string field;
  while (std::getline(ss, field, ',')) {
    fields.push_back(field);
  }
  return fields;
}

std::vector<QueryCase> load_query_file(
    const Options& options, geos::geom::GeometryFactory& factory,
    std::vector<GeometryPtr>& owned_queries) {
  std::ifstream input(options.query_file);
  if (!input) {
    throw std::runtime_error("Cannot open query file: " + options.query_file);
  }

  std::vector<QueryCase> queries;
  std::string line;
  while (std::getline(input, line)) {
    strip_cr(line);
    if (line.empty() || line.find("query_id") == 0) {
      continue;
    }

    std::vector<std::string> fields = split_csv_line(line);
    if (fields.size() < 6) {
      throw std::runtime_error("Bad query row: " + line);
    }

    QueryCase query;
    query.query_id = static_cast<std::size_t>(std::stoull(fields[0]));
    query.xmin = std::stod(fields[1]);
    query.ymin = std::stod(fields[2]);
    query.xmax = std::stod(fields[3]);
    query.ymax = std::stod(fields[4]);
    query.source_geometry_id =
        static_cast<std::size_t>(std::stoull(fields[5]));

    owned_queries.push_back(make_query_box(factory, query.xmin, query.ymin,
                                           query.xmax, query.ymax));
    query.geometry = owned_queries.back().get();
    queries.push_back(query);
    if (queries.size() >= options.query_count) {
      break;
    }
  }

  if (queries.empty()) {
    throw std::runtime_error("No query rows loaded from: " + options.query_file);
  }
  return queries;
}

std::size_t percentile_index(std::size_t size, double percentile) {
  if (size == 0) {
    return 0;
  }
  const double raw = percentile * static_cast<double>(size - 1);
  return static_cast<std::size_t>(std::ceil(raw));
}

long long percentile_value(std::vector<long long> values, double percentile) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  return values[percentile_index(values.size(), percentile)];
}

class DynamicExtentIndex {
 public:
  DynamicExtentIndex(const Options& options,
                     const std::vector<GeometryPtr>& geometries)
      : options_(options), geometries_(geometries) {}

  void bulk_load(const std::vector<ObjectId>& object_ids) {
    std::vector<RecordId> ids;
    ids.reserve(object_ids.size());
    for (ObjectId oid : object_ids) {
      RecordId rid = append_record(oid, geometries_[oid].get());
      ids.push_back(rid);
      object_to_record_[oid] = rid;
    }
    std::sort(ids.begin(), ids.end(), RecordIdLess(records_));

    for (std::size_t begin = 0; begin < ids.size(); begin += options_.block_size) {
      std::size_t end = std::min(begin + options_.block_size, ids.size());
      std::unique_ptr<DynamicBlock> block(new DynamicBlock());
      block->block_id = next_block_id_++;
      block->ids.assign(ids.begin() + begin, ids.begin() + end);
      DynamicBlock* block_ptr = block.get();
      owned_blocks_.push_back(std::move(block));
      directory_.push_back(block_ptr);
      for (RecordId rid : block_ptr->ids) {
        records_[rid].block = block_ptr;
      }
      rebuild_block_summary(block_ptr, false);
    }
  }

  RecordId insert(ObjectId oid) {
    RecordId rid = append_record(oid, geometries_[oid].get());
    DynamicBlock* block = find_block_for_zmin(records_[rid].zmin);
    if (block == nullptr) {
      block = create_empty_block();
    }

    auto pos = std::lower_bound(block->ids.begin(), block->ids.end(), rid,
                                RecordIdLess(records_));
    block->ids.insert(pos, rid);
    records_[rid].block = block;
    object_to_record_[oid] = rid;
    expand_summary_on_insert(block, records_[rid]);

    if (block->ids.size() > 2 * options_.block_size) {
      split_block(block);
    }
    return rid;
  }

  bool erase(ObjectId oid) {
    auto it = object_to_record_.find(oid);
    if (it == object_to_record_.end()) {
      return false;
    }
    RecordId rid = it->second;
    if (rid >= records_.size()) {
      object_to_record_.erase(it);
      return false;
    }

    DynamicRecord& record = records_[rid];
    if (!record.alive) {
      object_to_record_.erase(it);
      return false;
    }

    DynamicBlock* block = record.block;
    record.alive = false;
    object_to_record_.erase(it);

    if (block != nullptr) {
      if (block->live_count > 0) {
        --block->live_count;
      }
      ++block->dead_count;
      block->stale = true;
      if (should_rebuild(*block)) {
        rebuild_block_summary_and_compact(block);
      }
    }
    return true;
  }

  QueryResult query(const QueryCase& query_case) const {
    QueryResult result;
    double query_zmin = 0.0;
    double query_zmax = 0.0;
    geos::geom::Envelope query_envelope =
        *query_case.geometry->getEnvelopeInternal();

    auto start = std::chrono::high_resolution_clock::now();
    curve_shape_projection(query_case.geometry, "z", options_.cell_xmin,
                           options_.cell_ymin, options_.cell_size,
                           options_.cell_size, query_zmin, query_zmax);

    for (const DynamicBlock* block : directory_) {
      if (block->live_count == 0) {
        continue;
      }
      if (block->min_zmin > query_zmax) {
        break;
      }

      ++result.prefix_blocks;
      if (block->max_zmax < query_zmin) {
        ++result.skipped_zmax_blocks;
        continue;
      }
      if (!envelopes_intersect(block->mbr, query_envelope)) {
        ++result.skipped_mbr_blocks;
        continue;
      }

      ++result.visited_blocks;
      for (RecordId rid : block->ids) {
        const DynamicRecord& record = records_[rid];
        if (record.zmin > query_zmax) {
          break;
        }
        if (!record.alive) {
          continue;
        }

        ++result.records_scanned;
        if (record.zmax < query_zmin) {
          continue;
        }
        ++result.interval_candidates;
        if (!envelopes_intersect(record.envelope, query_envelope)) {
          continue;
        }
        ++result.mbr_candidates;
        ++result.exact_calls;
        if (query_case.geometry->intersects(record.geometry)) {
          result.answers.insert(record.object_id);
        }
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.query_ns = ns_count(end - start);
    return result;
  }

  bool validate_index(std::string* message = nullptr) const {
    auto fail = [&](const std::string& reason) {
      if (message != nullptr) {
        *message = reason;
      }
      return false;
    };

    std::unordered_set<RecordId> seen_live_records;
    bool have_previous_live_block = false;
    double previous_live_min_zmin = -std::numeric_limits<double>::infinity();
    for (const DynamicBlock* block : directory_) {
      for (std::size_t i = 1; i < block->ids.size(); ++i) {
        if (RecordIdLess(records_)(block->ids[i], block->ids[i - 1])) {
          return fail("block ids are not sorted by zmin");
        }
      }

      bool has_live = false;
      std::size_t true_live = 0;
      double true_min_zmin = std::numeric_limits<double>::infinity();
      double true_max_zmax = -std::numeric_limits<double>::infinity();
      geos::geom::Envelope true_mbr;

      for (RecordId rid : block->ids) {
        if (rid >= records_.size()) {
          return fail("block references out-of-range record id");
        }
        const DynamicRecord& record = records_[rid];
        if (record.block != block) {
          return fail("record points to a different block");
        }
        if (!record.alive) {
          continue;
        }
        ++true_live;
        seen_live_records.insert(rid);
        true_min_zmin = std::min(true_min_zmin, record.zmin);
        true_max_zmax = std::max(true_max_zmax, record.zmax);
        if (!has_live) {
          true_mbr = record.envelope;
          has_live = true;
        } else {
          true_mbr.expandToInclude(&record.envelope);
        }
      }

      if (block->live_count != true_live) {
        return fail("block live_count mismatch");
      }
      if (true_live == 0) {
        continue;
      }
      if (have_previous_live_block &&
          block->min_zmin + 1e-9 < previous_live_min_zmin) {
        return fail("directory live blocks are not sorted by min_zmin");
      }
      previous_live_min_zmin = block->min_zmin;
      have_previous_live_block = true;
      const double eps = 1e-9;
      if (block->max_zmax + eps < true_max_zmax) {
        return fail("block max_zmax is smaller than true live max_zmax");
      }
      if (block->min_zmin - eps > true_min_zmin) {
        return fail("block min_zmin is larger than true live min_zmin");
      }
      if (!envelope_contains(block->mbr, true_mbr)) {
        return fail("block MBR does not contain true live MBR");
      }
    }

    if (object_to_record_.size() != seen_live_records.size()) {
      return fail("object_to_record size does not match live record count");
    }

    for (const auto& item : object_to_record_) {
      ObjectId oid = item.first;
      RecordId rid = item.second;
      if (rid >= records_.size()) {
        return fail("object_to_record points out of range");
      }
      const DynamicRecord& record = records_[rid];
      if (!record.alive) {
        return fail("object_to_record points to dead record");
      }
      if (record.object_id != oid) {
        return fail("object_to_record object id mismatch");
      }
    }

    for (RecordId rid = 0; rid < records_.size(); ++rid) {
      const DynamicRecord& record = records_[rid];
      if (record.record_id != rid) {
        return fail("record_id field mismatch");
      }
      if (!record.alive) {
        continue;
      }
      if (seen_live_records.find(rid) == seen_live_records.end()) {
        return fail("live record was not found in any block");
      }
      auto it = object_to_record_.find(record.object_id);
      if (it == object_to_record_.end() || it->second != rid) {
        return fail("live record missing from object_to_record");
      }
    }
    return true;
  }

  std::vector<ObjectId> live_object_ids() const {
    std::vector<ObjectId> ids;
    ids.reserve(object_to_record_.size());
    for (const auto& item : object_to_record_) {
      ids.push_back(item.first);
    }
    return ids;
  }

  std::size_t live_count() const { return object_to_record_.size(); }
  std::size_t total_records() const { return records_.size(); }
  std::size_t dead_records() const { return records_.size() - live_count(); }
  std::size_t block_count() const { return directory_.size(); }
  std::size_t summary_rebuild_count() const { return summary_rebuild_count_; }
  long long summary_rebuild_ns() const { return summary_rebuild_ns_; }
  std::size_t block_split_count() const { return block_split_count_; }

  std::size_t stale_block_count() const {
    std::size_t count = 0;
    for (const DynamicBlock* block : directory_) {
      if (block->stale) {
        ++count;
      }
    }
    return count;
  }

  const std::vector<DynamicRecord>& records() const { return records_; }

 private:
  struct RecordIdLess {
    explicit RecordIdLess(const std::vector<DynamicRecord>& records)
        : records(records) {}

    bool operator()(RecordId lhs, RecordId rhs) const {
      const DynamicRecord& a = records[lhs];
      const DynamicRecord& b = records[rhs];
      if (a.zmin != b.zmin) {
        return a.zmin < b.zmin;
      }
      return a.record_id < b.record_id;
    }

    const std::vector<DynamicRecord>& records;
  };

  RecordId append_record(ObjectId oid, Geometry* geometry) {
    double zmin = 0.0;
    double zmax = 0.0;
    curve_shape_projection(geometry, "z", options_.cell_xmin, options_.cell_ymin,
                           options_.cell_size, options_.cell_size, zmin, zmax);

    RecordId rid = static_cast<RecordId>(records_.size());
    DynamicRecord record;
    record.object_id = oid;
    record.record_id = rid;
    record.zmin = zmin;
    record.zmax = zmax;
    record.envelope = *geometry->getEnvelopeInternal();
    record.geometry = geometry;
    record.alive = true;
    records_.push_back(record);
    return rid;
  }

  DynamicBlock* create_empty_block() {
    std::unique_ptr<DynamicBlock> block(new DynamicBlock());
    block->block_id = next_block_id_++;
    DynamicBlock* block_ptr = block.get();
    owned_blocks_.push_back(std::move(block));
    directory_.push_back(block_ptr);
    return block_ptr;
  }

  DynamicBlock* find_block_for_zmin(double zmin) const {
    if (directory_.empty()) {
      return nullptr;
    }
    auto it = std::lower_bound(
        directory_.begin(), directory_.end(), zmin,
        [](const DynamicBlock* block, double value) {
          return block->max_zmin < value;
        });
    if (it == directory_.end()) {
      return directory_.back();
    }
    return *it;
  }

  void expand_summary_on_insert(DynamicBlock* block,
                                const DynamicRecord& record) {
    if (block->live_count == 0) {
      block->min_zmin = record.zmin;
      block->max_zmin = record.zmin;
      block->max_zmax = record.zmax;
      block->mbr = record.envelope;
    } else {
      block->min_zmin = std::min(block->min_zmin, record.zmin);
      block->max_zmin = std::max(block->max_zmin, record.zmin);
      block->max_zmax = std::max(block->max_zmax, record.zmax);
      block->mbr.expandToInclude(&record.envelope);
    }
    ++block->live_count;
  }

  std::size_t stale_threshold_count() const {
    if (options_.stale_threshold_fraction <= 0.0) {
      return 0;
    }
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(options_.stale_threshold_fraction *
                         static_cast<double>(options_.block_size))));
  }

  bool should_rebuild(const DynamicBlock& block) const {
    std::size_t threshold = stale_threshold_count();
    if (threshold == 0) {
      return true;
    }
    return block.dead_count >= threshold;
  }

  void rebuild_block_summary(DynamicBlock* block, bool compact_dead) {
    bool has_live = false;
    std::size_t live_count = 0;
    std::vector<RecordId> live_ids;
    if (compact_dead) {
      live_ids.reserve(block->ids.size());
    }

    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;

    for (RecordId rid : block->ids) {
      DynamicRecord& record = records_[rid];
      if (!record.alive) {
        continue;
      }
      if (compact_dead) {
        live_ids.push_back(rid);
      }
      ++live_count;
      record.block = block;
      min_zmin = std::min(min_zmin, record.zmin);
      max_zmin = std::max(max_zmin, record.zmin);
      max_zmax = std::max(max_zmax, record.zmax);
      if (!has_live) {
        mbr = record.envelope;
        has_live = true;
      } else {
        mbr.expandToInclude(&record.envelope);
      }
    }

    if (compact_dead) {
      block->ids.swap(live_ids);
    }
    block->live_count = live_count;
    block->dead_count = 0;
    block->stale = false;
    if (has_live) {
      block->min_zmin = min_zmin;
      block->max_zmin = max_zmin;
      block->max_zmax = max_zmax;
      block->mbr = mbr;
    } else {
      block->min_zmin = std::numeric_limits<double>::infinity();
      block->max_zmin = -std::numeric_limits<double>::infinity();
      block->max_zmax = -std::numeric_limits<double>::infinity();
      block->mbr = geos::geom::Envelope();
    }
  }

  void rebuild_block_summary_and_compact(DynamicBlock* block) {
    auto start = std::chrono::high_resolution_clock::now();
    rebuild_block_summary(block, true);
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void split_block(DynamicBlock* block) {
    std::sort(block->ids.begin(), block->ids.end(), RecordIdLess(records_));
    std::size_t mid = block->ids.size() / 2;
    std::unique_ptr<DynamicBlock> right(new DynamicBlock());
    right->block_id = next_block_id_++;
    right->ids.assign(block->ids.begin() + mid, block->ids.end());
    block->ids.erase(block->ids.begin() + mid, block->ids.end());

    DynamicBlock* right_ptr = right.get();
    owned_blocks_.push_back(std::move(right));

    rebuild_block_summary(block, true);
    rebuild_block_summary(right_ptr, true);

    auto it = std::find(directory_.begin(), directory_.end(), block);
    if (it == directory_.end()) {
      directory_.push_back(right_ptr);
    } else {
      directory_.insert(it + 1, right_ptr);
    }
    ++block_split_count_;
  }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  std::vector<DynamicRecord> records_;
  std::vector<std::unique_ptr<DynamicBlock>> owned_blocks_;
  std::vector<DynamicBlock*> directory_;
  std::unordered_map<ObjectId, RecordId> object_to_record_;
  std::uint64_t next_block_id_ = 0;
  std::size_t summary_rebuild_count_ = 0;
  long long summary_rebuild_ns_ = 0;
  std::size_t block_split_count_ = 0;
};

RTree build_live_rtree(const DynamicExtentIndex& index, long long* build_ns) {
  auto start = std::chrono::high_resolution_clock::now();
  std::vector<RTreeValue> values;
  for (const DynamicRecord& record : index.records()) {
    if (!record.alive) {
      continue;
    }
    values.emplace_back(boost_box_from_envelope(record.envelope),
                        record.object_id);
  }
  RTree rtree(values.begin(), values.end());
  auto end = std::chrono::high_resolution_clock::now();
  if (build_ns != nullptr) {
    *build_ns = ns_count(end - start);
  }
  return rtree;
}

std::unordered_set<ObjectId> query_boost_exact(
    const RTree& rtree, const std::vector<GeometryPtr>& geometries,
    const QueryCase& query_case, long long* query_ns) {
  auto start = std::chrono::high_resolution_clock::now();
  std::unordered_set<ObjectId> answers;
  std::vector<RTreeValue> hits;
  geos::geom::Envelope query_envelope =
      *query_case.geometry->getEnvelopeInternal();
  rtree.query(bgi::intersects(boost_box_from_envelope(query_envelope)),
              std::back_inserter(hits));
  for (const auto& hit : hits) {
    ObjectId oid = hit.second;
    if (oid < geometries.size() &&
        query_case.geometry->intersects(geometries[oid].get())) {
      answers.insert(oid);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  if (query_ns != nullptr) {
    *query_ns = ns_count(end - start);
  }
  return answers;
}

void add_query_result(QueryAggregate& aggregate, const QueryResult& result) {
  aggregate.latencies_ns.push_back(result.query_ns);
  aggregate.prefix_blocks += result.prefix_blocks;
  aggregate.visited_blocks += result.visited_blocks;
  aggregate.skipped_zmax_blocks += result.skipped_zmax_blocks;
  aggregate.skipped_mbr_blocks += result.skipped_mbr_blocks;
  aggregate.records_scanned += result.records_scanned;
  aggregate.interval_candidates += result.interval_candidates;
  aggregate.mbr_candidates += result.mbr_candidates;
  aggregate.exact_calls += result.exact_calls;
  aggregate.answers += result.answers.size();
  if (result.answers.empty()) {
    ++aggregate.zero_answer_queries;
  }
}

std::size_t count_missing(const std::unordered_set<ObjectId>& expected,
                          const std::unordered_set<ObjectId>& actual,
                          ObjectId* first_missing) {
  std::size_t count = 0;
  for (ObjectId oid : expected) {
    if (actual.find(oid) == actual.end()) {
      if (count == 0 && first_missing != nullptr) {
        *first_missing = oid;
      }
      ++count;
    }
  }
  return count;
}

struct CompareSummary {
  std::string workload_mode = "staged";
  std::string mixed_profile;
  std::size_t operation_count = 0;
  std::size_t checkpoint_ops = 0;
  double mixed_query_ratio = 0.0;
  double mixed_insert_ratio = 0.0;
  double mixed_delete_ratio = 0.0;
  std::string index;
  std::string checkpoint;
  std::size_t checkpoint_id = 0;
  std::size_t loaded_count = 0;
  std::size_t initial_count = 0;
  std::size_t insert_count = 0;
  std::size_t delete_count = 0;
  std::size_t live_count = 0;
  std::size_t query_count = 0;
  long long build_ns = 0;
  long long insert_ns = 0;
  long long delete_ns = 0;
  long long p95_insert_ns = 0;
  long long p99_insert_ns = 0;
  long long p95_delete_ns = 0;
  long long p99_delete_ns = 0;
  double insert_tps = 0.0;
  double delete_tps = 0.0;
  double query_tps = 0.0;
  double overall_ops_tps = 0.0;
  long long avg_query_ns = 0;
  long long p50_query_ns = 0;
  long long p95_query_ns = 0;
  long long p99_query_ns = 0;
  std::size_t candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t answers = 0;
  double candidate_answer_ratio = 0.0;
  std::size_t zero_answer_queries = 0;
  int answers_match_boost = 1;
  std::size_t missing_count = 0;
  std::size_t extra_count = 0;
  long long oracle_build_ns = 0;
  long long oracle_query_ns = 0;
  std::size_t block_size = 0;
  double stale_threshold_fraction = 0.0;
  double delete_compact_fraction = 0.0;
  std::size_t block_count = 0;
  std::size_t stale_block_count = 0;
  std::size_t summary_rebuild_count = 0;
  long long summary_rebuild_ns = 0;
  std::size_t summary_rebuild_count_total = 0;
  long long summary_rebuild_ns_total = 0;
  std::size_t summary_rebuild_count_stage = 0;
  long long summary_rebuild_ns_stage = 0;
  std::size_t block_split_count = 0;
  std::size_t local_compaction_count = 0;
  long long local_compaction_ns = 0;
  std::size_t local_compaction_count_total = 0;
  long long local_compaction_ns_total = 0;
  std::size_t local_compaction_count_stage = 0;
  long long local_compaction_ns_stage = 0;
  double avg_local_compaction_us_stage = 0.0;
  std::size_t local_delta_bound = 0;
  std::size_t delete_compact_bound = 0;
  std::size_t max_local_delta_size = 0;
  double avg_local_delta_size = 0.0;
  std::size_t blocks_with_delta = 0;
  double tombstone_ratio = 0.0;
  std::size_t pieces = 0;
  std::size_t tree_size = 0;
  std::size_t tree_depth = 0;
  std::size_t tree_nodes = 0;
  std::size_t index_bytes_estimate = 0;
  std::size_t success_count = 0;
  std::size_t failed_count = 0;
  int validate_ok = 1;
  std::string note;
};

struct SimpleQueryResult {
  long long query_ns = 0;
  std::size_t candidates = 0;
  std::size_t exact_calls = 0;
  std::unordered_set<ObjectId> answers;
};

struct SimpleQueryAggregate {
  std::vector<long long> latencies_ns;
  std::size_t candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t answers = 0;
  std::size_t zero_answer_queries = 0;
};

void add_simple_query_result(SimpleQueryAggregate& aggregate,
                             const SimpleQueryResult& result) {
  aggregate.latencies_ns.push_back(result.query_ns);
  aggregate.candidates += result.candidates;
  aggregate.exact_calls += result.exact_calls;
  aggregate.answers += result.answers.size();
  if (result.answers.empty()) {
    ++aggregate.zero_answer_queries;
  }
}

class DeliAlexIndex {
 public:
  using BaseAlex = alex::Alex<double, Geometry*>;
  using GlinIndex = alex::Glin<double, Geometry*>;
  using Leaf = typename BaseAlex::data_node_type;
  using ModelNode = typename BaseAlex::model_node_type;

  DeliAlexIndex(const Options& options,
                const std::vector<GeometryPtr>& geometries,
                const std::vector<GeometryMeta>& metadata)
      : options_(options), geometries_(geometries), metadata_(metadata) {
    live_.assign(geometries.size(), 0);
    geometry_to_meta_.reserve(geometries.size());
    for (const GeometryMeta& meta : metadata_) {
      if (meta.object_id < geometries_.size()) {
        geometry_to_meta_[geometries_[meta.object_id].get()] = &meta;
      }
    }
  }

  void bulk_load(const std::vector<ObjectId>& ids) {
    std::vector<std::pair<double, Geometry*>> values;
    values.reserve(ids.size());
    for (ObjectId oid : ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      values.emplace_back(metadata_[oid].zmin, geometries_[oid].get());
      live_[oid] = 1;
    }
    std::sort(values.begin(), values.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                  return lhs.first < rhs.first;
                }
                return lhs.second < rhs.second;
              });
    base().bulk_load(values.data(), static_cast<int>(values.size()));
    rebuild_all_summaries();
    mark_directory_dirty();
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    const GeometryMeta& meta = metadata_[oid];
    const int leaves_before = base().num_leaves();
    auto result = base().insert(meta.zmin, geometries_[oid].get());
    live_[oid] = result.second ? 1 : live_[oid];
    const int leaves_after = base().num_leaves();
    if (leaves_after != leaves_before || result.first.cur_leaf_ == nullptr ||
        summaries_.find(result.first.cur_leaf_) == summaries_.end()) {
      refresh_local_summaries(meta.zmin);
    } else {
      expand_summary_on_insert(result.first.cur_leaf_, meta);
    }
    mark_directory_dirty();
    return result.second;
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size() || !live_[oid]) {
      return false;
    }
    const GeometryMeta& meta = metadata_[oid];
    const int leaves_before = base().num_leaves();
    const Leaf* touched_leaf = base().get_leaf(meta.zmin);
    int erased = base().erase_geo(meta.zmin, geometries_[oid].get());
    if (erased <= 0) {
      return false;
    }
    live_[oid] = 0;
    const int leaves_after = base().num_leaves();
    if (leaves_after != leaves_before) {
      summaries_.erase(touched_leaf);
    } else {
      mark_summary_stale(touched_leaf);
    }
    mark_directory_dirty();
    return true;
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    auto start = std::chrono::high_resolution_clock::now();
    SimpleQueryResult result;

    double query_zmin = 0.0;
    double query_zmax = 0.0;
    curve_shape_projection(query_case.geometry, "z", options_.cell_xmin,
                           options_.cell_ymin, options_.cell_size,
                           options_.cell_size, query_zmin, query_zmax);
    const geos::geom::Envelope query_envelope =
        *query_case.geometry->getEnvelopeInternal();

    ensure_group_directory();
    for (const LeafGroupSummary& group : group_directory_) {
      if (!group.has_live) {
        continue;
      }
      if (group.min_zmin > query_zmax) {
        break;
      }
      if (group.max_zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(group.mbr, query_envelope)) {
        continue;
      }

      for (const Leaf* leaf = group.first_leaf; leaf != nullptr;
           leaf = leaf->next_leaf_) {
        auto summary_it = summaries_.find(leaf);
        if (summary_it != summaries_.end()) {
          const LeafSummary& summary = summary_it->second;
          if (!summary.has_live) {
            if (leaf == group.last_leaf) {
              break;
            }
            continue;
          }
          if (summary.min_zmin > query_zmax) {
            break;
          }
          if (summary.max_zmax < query_zmin) {
            if (leaf == group.last_leaf) {
              break;
            }
            continue;
          }
          if (!envelopes_intersect(summary.mbr, query_envelope)) {
            if (leaf == group.last_leaf) {
              break;
            }
            continue;
          }
        }

        for (int pos = 0; pos < leaf->data_capacity_; ++pos) {
          if (!leaf->check_exists(pos)) {
            continue;
          }
          double zmin = leaf->get_key(pos);
          if (zmin > query_zmax) {
            break;
          }
          Geometry* geometry = leaf->get_payload(pos);
          const GeometryMeta* meta = find_meta(geometry);
          if (meta == nullptr || meta->object_id >= live_.size() ||
              !live_[meta->object_id]) {
            continue;
          }
          if (meta->zmax < query_zmin) {
            continue;
          }
          if (!envelopes_intersect(meta->envelope, query_envelope)) {
            continue;
          }
          ++result.candidates;
          ++result.exact_calls;
          if (query_case.geometry->intersects(geometry)) {
            result.answers.insert(meta->object_id);
          }
        }

        if (leaf == group.last_leaf) {
          break;
        }
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.query_ns = ns_count(end - start);
    return result;
  }

  std::size_t leaf_count() const {
    return static_cast<std::size_t>(std::max(0, base().num_leaves()));
  }

  std::size_t group_count() const {
    ensure_group_directory();
    return group_directory_.size();
  }

  std::size_t stale_leaf_count() const {
    std::size_t count = 0;
    for (const auto& item : summaries_) {
      if (item.second.stale) {
        ++count;
      }
    }
    return count;
  }

  std::size_t summary_rebuild_count() const { return summary_rebuild_count_; }
  long long summary_rebuild_ns() const { return summary_rebuild_ns_; }
  std::size_t group_directory_rebuild_count() const {
    return group_directory_rebuild_count_;
  }
  long long group_directory_rebuild_ns() const {
    return group_directory_rebuild_ns_;
  }

  std::size_t index_bytes_estimate() const {
    ensure_group_directory();
    return static_cast<std::size_t>(
        std::max<long long>(0, base().data_size() + base().model_size())) +
           summaries_.size() * sizeof(LeafSummary) +
           group_directory_.size() * sizeof(LeafGroupSummary) +
           live_.size() * sizeof(char) +
           geometry_to_meta_.size() *
               (sizeof(Geometry*) + sizeof(const GeometryMeta*));
  }

 private:
  struct LeafSummary {
    bool has_live = false;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;
    std::size_t live_count = 0;
    bool stale = false;
  };

  struct LeafGroupSummary {
    const Leaf* first_leaf = nullptr;
    const Leaf* last_leaf = nullptr;
    bool has_live = false;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;
    std::size_t live_count = 0;
    std::size_t leaf_count = 0;
    bool stale = false;
  };

  BaseAlex& base() { return static_cast<BaseAlex&>(index_); }
  const BaseAlex& base() const { return static_cast<const BaseAlex&>(index_); }

  Leaf* first_leaf() {
    alex::AlexNode<double, Geometry*>* node = base().root_node_;
    if (node == nullptr) {
      return nullptr;
    }
    while (!node->is_leaf_) {
      node = static_cast<ModelNode*>(node)->children_[0];
    }
    return static_cast<Leaf*>(node);
  }

  const Leaf* first_leaf() const {
    alex::AlexNode<double, Geometry*>* node = base().root_node_;
    if (node == nullptr) {
      return nullptr;
    }
    while (!node->is_leaf_) {
      node = static_cast<ModelNode*>(node)->children_[0];
    }
    return static_cast<const Leaf*>(node);
  }

  const GeometryMeta* find_meta(Geometry* geometry) const {
    auto it = geometry_to_meta_.find(geometry);
    if (it == geometry_to_meta_.end()) {
      return nullptr;
    }
    return it->second;
  }

  LeafSummary recompute_leaf_summary(const Leaf* leaf) const {
    LeafSummary summary;
    for (int pos = 0; pos < leaf->data_capacity_; ++pos) {
      if (!leaf->check_exists(pos)) {
        continue;
      }
      Geometry* geometry = leaf->get_payload(pos);
      const GeometryMeta* meta = find_meta(geometry);
      if (meta == nullptr || meta->object_id >= live_.size() ||
          !live_[meta->object_id]) {
        continue;
      }
      summary.has_live = true;
      ++summary.live_count;
      summary.min_zmin = std::min(summary.min_zmin, meta->zmin);
      summary.max_zmin = std::max(summary.max_zmin, meta->zmin);
      summary.max_zmax = std::max(summary.max_zmax, meta->zmax);
      if (summary.live_count == 1) {
        summary.mbr = meta->envelope;
      } else {
        summary.mbr.expandToInclude(&meta->envelope);
      }
    }
    return summary;
  }

  void rebuild_all_summaries() {
    auto start = std::chrono::high_resolution_clock::now();
    summaries_.clear();
    for (Leaf* leaf = first_leaf(); leaf != nullptr; leaf = leaf->next_leaf_) {
      summaries_[leaf] = recompute_leaf_summary(leaf);
    }
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void refresh_local_summaries(double zmin) {
    auto start = std::chrono::high_resolution_clock::now();
    Leaf* center = base().get_leaf(zmin);
    if (center == nullptr) {
      auto end = std::chrono::high_resolution_clock::now();
      ++summary_rebuild_count_;
      summary_rebuild_ns_ += ns_count(end - start);
      return;
    }

    Leaf* begin = center;
    for (int i = 0; i < 2 && begin->prev_leaf_ != nullptr; ++i) {
      begin = begin->prev_leaf_;
    }
    Leaf* end_leaf = center;
    for (int i = 0; i < 2 && end_leaf->next_leaf_ != nullptr; ++i) {
      end_leaf = end_leaf->next_leaf_;
    }

    for (Leaf* leaf = begin; leaf != nullptr; leaf = leaf->next_leaf_) {
      summaries_[leaf] = recompute_leaf_summary(leaf);
      if (leaf == end_leaf) {
        break;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void expand_summary_on_insert(const Leaf* leaf, const GeometryMeta& meta) {
    LeafSummary& summary = summaries_[leaf];
    if (!summary.has_live) {
      summary.has_live = true;
      summary.min_zmin = meta.zmin;
      summary.max_zmin = meta.zmin;
      summary.max_zmax = meta.zmax;
      summary.mbr = meta.envelope;
      summary.live_count = 1;
      summary.stale = false;
      return;
    }
    summary.min_zmin = std::min(summary.min_zmin, meta.zmin);
    summary.max_zmin = std::max(summary.max_zmin, meta.zmin);
    summary.max_zmax = std::max(summary.max_zmax, meta.zmax);
    summary.mbr.expandToInclude(&meta.envelope);
    ++summary.live_count;
  }

  void mark_summary_stale(const Leaf* leaf) {
    auto it = summaries_.find(leaf);
    if (it != summaries_.end()) {
      it->second.stale = true;
    }
  }

  void mark_directory_dirty() const { group_directory_dirty_ = true; }

  void ensure_group_directory() const {
    if (!group_directory_dirty_) {
      return;
    }
    rebuild_group_directory();
  }

  void add_leaf_to_group(LeafGroupSummary& group, const Leaf* leaf,
                         const LeafSummary& summary) const {
    if (group.first_leaf == nullptr) {
      group.first_leaf = leaf;
    }
    group.last_leaf = leaf;
    ++group.leaf_count;
    group.stale = group.stale || summary.stale;
    if (!summary.has_live) {
      return;
    }
    if (!group.has_live) {
      group.has_live = true;
      group.min_zmin = summary.min_zmin;
      group.max_zmin = summary.max_zmin;
      group.max_zmax = summary.max_zmax;
      group.mbr = summary.mbr;
      group.live_count = summary.live_count;
      return;
    }
    group.min_zmin = std::min(group.min_zmin, summary.min_zmin);
    group.max_zmin = std::max(group.max_zmin, summary.max_zmin);
    group.max_zmax = std::max(group.max_zmax, summary.max_zmax);
    group.mbr.expandToInclude(&summary.mbr);
    group.live_count += summary.live_count;
  }

  void rebuild_group_directory() const {
    auto start = std::chrono::high_resolution_clock::now();
    group_directory_.clear();
    const std::size_t target_records =
        std::max<std::size_t>(1, options_.block_size);

    LeafGroupSummary current;
    for (const Leaf* leaf = first_leaf(); leaf != nullptr;
         leaf = leaf->next_leaf_) {
      auto it = summaries_.find(leaf);
      if (it == summaries_.end()) {
        continue;
      }
      add_leaf_to_group(current, leaf, it->second);
      if (current.live_count >= target_records) {
        group_directory_.push_back(current);
        current = LeafGroupSummary();
      }
    }
    if (current.first_leaf != nullptr) {
      group_directory_.push_back(current);
    }

    group_directory_dirty_ = false;
    auto end = std::chrono::high_resolution_clock::now();
    ++group_directory_rebuild_count_;
    group_directory_rebuild_ns_ += ns_count(end - start);
  }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  GlinIndex index_;
  std::vector<char> live_;
  std::unordered_map<Geometry*, const GeometryMeta*> geometry_to_meta_;
  std::unordered_map<const Leaf*, LeafSummary> summaries_;
  mutable std::vector<LeafGroupSummary> group_directory_;
  mutable bool group_directory_dirty_ = true;
  mutable std::size_t group_directory_rebuild_count_ = 0;
  mutable long long group_directory_rebuild_ns_ = 0;
  std::size_t summary_rebuild_count_ = 0;
  long long summary_rebuild_ns_ = 0;
};

struct CompactOverlayBlock {
  std::uint64_t block_id = 0;
  std::vector<ObjectId> ids;
  double min_zmin = std::numeric_limits<double>::infinity();
  double max_zmin = -std::numeric_limits<double>::infinity();
  double max_zmax = -std::numeric_limits<double>::infinity();
  geos::geom::Envelope mbr;
  std::size_t live_count = 0;
  std::size_t dead_count = 0;
  bool stale = false;
};

class CompactQueryOverlay {
 public:
  CompactQueryOverlay(const Options& options,
                      const std::vector<GeometryPtr>& geometries,
                      const std::vector<GeometryMeta>& metadata)
      : options_(options), geometries_(geometries), metadata_(metadata) {
    live_.assign(geometries.size(), 0);
    object_to_block_.assign(geometries.size(), nullptr);
  }

  void bulk_load(const std::vector<ObjectId>& object_ids) {
    std::vector<ObjectId> ids;
    ids.reserve(object_ids.size());
    for (ObjectId oid : object_ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      ids.push_back(oid);
      live_[oid] = 1;
    }
    std::sort(ids.begin(), ids.end(), ObjectIdLess(metadata_));

    for (std::size_t begin = 0; begin < ids.size();
         begin += options_.block_size) {
      std::size_t end = std::min(begin + options_.block_size, ids.size());
      std::unique_ptr<CompactOverlayBlock> block(new CompactOverlayBlock());
      block->block_id = next_block_id_++;
      block->ids.assign(ids.begin() + begin, ids.begin() + end);
      CompactOverlayBlock* block_ptr = block.get();
      owned_blocks_.push_back(std::move(block));
      directory_.push_back(block_ptr);
      for (ObjectId oid : block_ptr->ids) {
        object_to_block_[oid] = block_ptr;
      }
      rebuild_block_summary(block_ptr, false);
    }
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size() || live_[oid]) {
      return false;
    }
    CompactOverlayBlock* block = find_block_for_zmin(metadata_[oid].zmin);
    if (block == nullptr) {
      block = create_empty_block();
    }
    auto pos = std::lower_bound(block->ids.begin(), block->ids.end(), oid,
                                ObjectIdLess(metadata_));
    block->ids.insert(pos, oid);
    live_[oid] = 1;
    object_to_block_[oid] = block;
    expand_summary_on_insert(block, metadata_[oid]);
    if (block->ids.size() > 2 * options_.block_size) {
      split_block(block);
    }
    return true;
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size() || !live_[oid]) {
      return false;
    }
    CompactOverlayBlock* block = object_to_block_[oid];
    live_[oid] = 0;
    object_to_block_[oid] = nullptr;
    if (block != nullptr) {
      if (block->live_count > 0) {
        --block->live_count;
      }
      ++block->dead_count;
      block->stale = true;
      if (should_rebuild(*block)) {
        rebuild_block_summary_and_compact(block);
      }
    }
    return true;
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    auto start = std::chrono::high_resolution_clock::now();
    SimpleQueryResult result;

    double query_zmin = 0.0;
    double query_zmax = 0.0;
    curve_shape_projection(query_case.geometry, "z", options_.cell_xmin,
                           options_.cell_ymin, options_.cell_size,
                           options_.cell_size, query_zmin, query_zmax);
    const geos::geom::Envelope query_envelope =
        *query_case.geometry->getEnvelopeInternal();

    for (const CompactOverlayBlock* block : directory_) {
      if (block->live_count == 0) {
        continue;
      }
      if (block->min_zmin > query_zmax) {
        break;
      }
      if (block->max_zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(block->mbr, query_envelope)) {
        continue;
      }
      for (ObjectId oid : block->ids) {
        if (oid >= metadata_.size()) {
          continue;
        }
        const GeometryMeta& meta = metadata_[oid];
        if (meta.zmin > query_zmax) {
          break;
        }
        if (!live_[oid]) {
          continue;
        }
        if (meta.zmax < query_zmin) {
          continue;
        }
        if (!envelopes_intersect(meta.envelope, query_envelope)) {
          continue;
        }
        ++result.candidates;
        ++result.exact_calls;
        if (query_case.geometry->intersects(geometries_[oid].get())) {
          result.answers.insert(oid);
        }
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.query_ns = ns_count(end - start);
    return result;
  }

  bool validate_index(std::string* message = nullptr) const {
    auto fail = [&](const std::string& reason) {
      if (message != nullptr) {
        *message = reason;
      }
      return false;
    };

    std::size_t seen_live = 0;
    bool have_previous_live_block = false;
    double previous_live_min_zmin = -std::numeric_limits<double>::infinity();
    for (const CompactOverlayBlock* block : directory_) {
      for (std::size_t i = 1; i < block->ids.size(); ++i) {
        if (ObjectIdLess(metadata_)(block->ids[i], block->ids[i - 1])) {
          return fail("compact overlay block ids are not sorted by zmin");
        }
      }

      bool has_live = false;
      std::size_t true_live = 0;
      double true_min_zmin = std::numeric_limits<double>::infinity();
      double true_max_zmax = -std::numeric_limits<double>::infinity();
      geos::geom::Envelope true_mbr;
      for (ObjectId oid : block->ids) {
        if (oid >= live_.size()) {
          return fail("compact overlay block references out-of-range object id");
        }
        if (!live_[oid]) {
          continue;
        }
        if (object_to_block_[oid] != block) {
          return fail("compact overlay object points to a different block");
        }
        const GeometryMeta& meta = metadata_[oid];
        ++true_live;
        ++seen_live;
        true_min_zmin = std::min(true_min_zmin, meta.zmin);
        true_max_zmax = std::max(true_max_zmax, meta.zmax);
        if (!has_live) {
          true_mbr = meta.envelope;
          has_live = true;
        } else {
          true_mbr.expandToInclude(&meta.envelope);
        }
      }

      if (block->live_count != true_live) {
        return fail("compact overlay block live_count mismatch");
      }
      if (true_live == 0) {
        continue;
      }
      if (have_previous_live_block &&
          block->min_zmin + 1e-9 < previous_live_min_zmin) {
        return fail("compact overlay directory is not sorted by min_zmin");
      }
      previous_live_min_zmin = block->min_zmin;
      have_previous_live_block = true;
      const double eps = 1e-9;
      if (block->max_zmax + eps < true_max_zmax) {
        return fail("compact overlay max_zmax is smaller than true max_zmax");
      }
      if (block->min_zmin - eps > true_min_zmin) {
        return fail("compact overlay min_zmin is larger than true min_zmin");
      }
      if (!envelope_contains(block->mbr, true_mbr)) {
        return fail("compact overlay MBR does not contain true MBR");
      }
    }

    if (seen_live != live_count()) {
      return fail("compact overlay live object count mismatch");
    }
    return true;
  }

  std::size_t live_count() const {
    return static_cast<std::size_t>(
        std::count(live_.begin(), live_.end(), static_cast<char>(1)));
  }
  std::size_t total_records() const { return live_.size(); }
  std::size_t block_count() const { return directory_.size(); }
  std::size_t summary_rebuild_count() const { return summary_rebuild_count_; }
  long long summary_rebuild_ns() const { return summary_rebuild_ns_; }
  std::size_t block_split_count() const { return block_split_count_; }

  std::size_t stale_block_count() const {
    std::size_t count = 0;
    for (const CompactOverlayBlock* block : directory_) {
      if (block->stale) {
        ++count;
      }
    }
    return count;
  }

  std::size_t index_bytes_estimate() const {
    std::size_t bytes = live_.size() * sizeof(char);
    bytes += object_to_block_.size() * sizeof(CompactOverlayBlock*);
    bytes += directory_.size() * sizeof(CompactOverlayBlock*);
    bytes += owned_blocks_.size() * sizeof(CompactOverlayBlock);
    for (const auto& block : owned_blocks_) {
      bytes += block->ids.capacity() * sizeof(ObjectId);
    }
    return bytes;
  }

 private:
  struct ObjectIdLess {
    explicit ObjectIdLess(const std::vector<GeometryMeta>& metadata)
        : metadata(metadata) {}

    bool operator()(ObjectId lhs, ObjectId rhs) const {
      const GeometryMeta& a = metadata[lhs];
      const GeometryMeta& b = metadata[rhs];
      if (a.zmin != b.zmin) {
        return a.zmin < b.zmin;
      }
      return lhs < rhs;
    }

    const std::vector<GeometryMeta>& metadata;
  };

  CompactOverlayBlock* create_empty_block() {
    std::unique_ptr<CompactOverlayBlock> block(new CompactOverlayBlock());
    block->block_id = next_block_id_++;
    CompactOverlayBlock* block_ptr = block.get();
    owned_blocks_.push_back(std::move(block));
    directory_.push_back(block_ptr);
    return block_ptr;
  }

  CompactOverlayBlock* find_block_for_zmin(double zmin) const {
    if (directory_.empty()) {
      return nullptr;
    }
    auto it = std::lower_bound(
        directory_.begin(), directory_.end(), zmin,
        [](const CompactOverlayBlock* block, double value) {
          return block->max_zmin < value;
        });
    if (it == directory_.end()) {
      return directory_.back();
    }
    return *it;
  }

  void expand_summary_on_insert(CompactOverlayBlock* block,
                                const GeometryMeta& meta) {
    if (block->live_count == 0) {
      block->min_zmin = meta.zmin;
      block->max_zmin = meta.zmin;
      block->max_zmax = meta.zmax;
      block->mbr = meta.envelope;
    } else {
      block->min_zmin = std::min(block->min_zmin, meta.zmin);
      block->max_zmin = std::max(block->max_zmin, meta.zmin);
      block->max_zmax = std::max(block->max_zmax, meta.zmax);
      block->mbr.expandToInclude(&meta.envelope);
    }
    ++block->live_count;
  }

  std::size_t stale_threshold_count() const {
    if (options_.stale_threshold_fraction <= 0.0) {
      return 0;
    }
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(options_.stale_threshold_fraction *
                         static_cast<double>(options_.block_size))));
  }

  bool should_rebuild(const CompactOverlayBlock& block) const {
    std::size_t threshold = stale_threshold_count();
    if (threshold == 0) {
      return true;
    }
    return block.dead_count >= threshold;
  }

  void rebuild_block_summary(CompactOverlayBlock* block, bool compact_dead) {
    bool has_live = false;
    std::size_t live_count = 0;
    std::vector<ObjectId> live_ids;
    if (compact_dead) {
      live_ids.reserve(block->ids.size());
    }

    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;

    for (ObjectId oid : block->ids) {
      if (oid >= live_.size() || !live_[oid]) {
        continue;
      }
      if (compact_dead) {
        live_ids.push_back(oid);
      }
      object_to_block_[oid] = block;
      const GeometryMeta& meta = metadata_[oid];
      ++live_count;
      min_zmin = std::min(min_zmin, meta.zmin);
      max_zmin = std::max(max_zmin, meta.zmin);
      max_zmax = std::max(max_zmax, meta.zmax);
      if (!has_live) {
        mbr = meta.envelope;
        has_live = true;
      } else {
        mbr.expandToInclude(&meta.envelope);
      }
    }

    if (compact_dead) {
      block->ids.swap(live_ids);
    }
    block->live_count = live_count;
    block->dead_count = 0;
    block->stale = false;
    if (has_live) {
      block->min_zmin = min_zmin;
      block->max_zmin = max_zmin;
      block->max_zmax = max_zmax;
      block->mbr = mbr;
    } else {
      block->min_zmin = std::numeric_limits<double>::infinity();
      block->max_zmin = -std::numeric_limits<double>::infinity();
      block->max_zmax = -std::numeric_limits<double>::infinity();
      block->mbr = geos::geom::Envelope();
    }
  }

  void rebuild_block_summary_and_compact(CompactOverlayBlock* block) {
    auto start = std::chrono::high_resolution_clock::now();
    rebuild_block_summary(block, true);
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void split_block(CompactOverlayBlock* block) {
    std::sort(block->ids.begin(), block->ids.end(), ObjectIdLess(metadata_));
    std::size_t mid = block->ids.size() / 2;
    std::unique_ptr<CompactOverlayBlock> right(new CompactOverlayBlock());
    right->block_id = next_block_id_++;
    right->ids.assign(block->ids.begin() + mid, block->ids.end());
    block->ids.erase(block->ids.begin() + mid, block->ids.end());

    CompactOverlayBlock* right_ptr = right.get();
    owned_blocks_.push_back(std::move(right));

    rebuild_block_summary(block, true);
    rebuild_block_summary(right_ptr, true);

    auto it = std::find(directory_.begin(), directory_.end(), block);
    if (it == directory_.end()) {
      directory_.push_back(right_ptr);
    } else {
      directory_.insert(it + 1, right_ptr);
    }
    ++block_split_count_;
  }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  std::vector<char> live_;
  std::vector<CompactOverlayBlock*> object_to_block_;
  std::vector<std::unique_ptr<CompactOverlayBlock>> owned_blocks_;
  std::vector<CompactOverlayBlock*> directory_;
  std::uint64_t next_block_id_ = 0;
  std::size_t summary_rebuild_count_ = 0;
  long long summary_rebuild_ns_ = 0;
  std::size_t block_split_count_ = 0;
};

class DeliAlexHybridIndex {
 public:
  using BaseAlex = alex::Alex<double, Geometry*>;
  using GlinIndex = alex::Glin<double, Geometry*>;

  DeliAlexHybridIndex(const Options& options,
                      const std::vector<GeometryPtr>& geometries,
                      const std::vector<GeometryMeta>& metadata)
      : options_(options),
        geometries_(geometries),
        metadata_(metadata),
        overlay_(options, geometries, metadata) {}

  void bulk_load(const std::vector<ObjectId>& ids) {
    std::vector<std::pair<double, Geometry*>> values;
    values.reserve(ids.size());
    for (ObjectId oid : ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      values.emplace_back(metadata_[oid].zmin, geometries_[oid].get());
    }
    std::sort(values.begin(), values.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                  return lhs.first < rhs.first;
                }
                return lhs.second < rhs.second;
              });
    base().bulk_load(values.data(), static_cast<int>(values.size()));
    overlay_.bulk_load(ids);
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    auto result = base().insert(metadata_[oid].zmin, geometries_[oid].get());
    if (!result.second) {
      return false;
    }
    return overlay_.insert(oid);
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    int erased = base().erase_geo(metadata_[oid].zmin, geometries_[oid].get());
    if (erased <= 0) {
      return false;
    }
    return overlay_.erase(oid);
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    return overlay_.query(query_case);
  }

  bool validate_index(std::string* message = nullptr) const {
    return overlay_.validate_index(message);
  }

  std::size_t live_count() const { return overlay_.live_count(); }
  std::size_t block_count() const { return overlay_.block_count(); }
  std::size_t stale_block_count() const { return overlay_.stale_block_count(); }
  std::size_t summary_rebuild_count() const {
    return overlay_.summary_rebuild_count();
  }
  long long summary_rebuild_ns() const { return overlay_.summary_rebuild_ns(); }
  std::size_t block_split_count() const { return overlay_.block_split_count(); }
  std::size_t leaf_count() const {
    return static_cast<std::size_t>(std::max(0, base().num_leaves()));
  }
  std::size_t index_bytes_estimate() const {
    return static_cast<std::size_t>(
               std::max<long long>(0, base().data_size() + base().model_size())) +
           overlay_.index_bytes_estimate();
  }

 private:
  BaseAlex& base() { return static_cast<BaseAlex&>(index_); }
  const BaseAlex& base() const { return static_cast<const BaseAlex&>(index_); }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  GlinIndex index_;
  CompactQueryOverlay overlay_;
};

class BufferedCompactQueryOverlay {
 public:
  BufferedCompactQueryOverlay(const Options& options,
                              const std::vector<GeometryPtr>& geometries,
                              const std::vector<GeometryMeta>& metadata,
                              bool bounded_delta = false)
      : options_(options),
        geometries_(geometries),
        metadata_(metadata),
        bounded_delta_(bounded_delta) {
    live_.assign(geometries.size(), 0);
    open_block_.block_id = next_block_id_++;
  }

  void bulk_load(const std::vector<ObjectId>& object_ids) {
    std::vector<ObjectId> ids;
    ids.reserve(object_ids.size());
    for (ObjectId oid : object_ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      ids.push_back(oid);
      live_[oid] = 1;
    }
    std::sort(ids.begin(), ids.end(), ObjectIdLess(metadata_));

    for (std::size_t begin = 0; begin < ids.size();
         begin += options_.block_size) {
      std::size_t end = std::min(begin + options_.block_size, ids.size());
      std::unique_ptr<CompactOverlayBlock> block(new CompactOverlayBlock());
      block->block_id = next_block_id_++;
      block->ids.assign(ids.begin() + begin, ids.begin() + end);
      rebuild_block_summary(block.get());
      main_blocks_.push_back(std::move(block));
    }
    adaptive_delta_record_bound_ = estimate_adaptive_delta_record_bound();
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size() || live_[oid]) {
      return false;
    }
    live_[oid] = 1;
    open_block_.ids.push_back(oid);
    ++delta_record_count_;
    expand_summary_on_insert(open_block_, metadata_[oid]);
    if (open_block_.ids.size() >= options_.block_size) {
      seal_open_block();
    }
    if (bounded_delta_ &&
        delta_record_count_ >= adaptive_delta_record_bound_) {
      promote_delta_blocks_to_main();
    }
    return true;
  }

  bool erase(ObjectId oid) {
    if (oid >= live_.size() || !live_[oid]) {
      return false;
    }
    live_[oid] = 0;
    ++dead_count_;
    return true;
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    auto start = std::chrono::high_resolution_clock::now();
    SimpleQueryResult result;

    double query_zmin = 0.0;
    double query_zmax = 0.0;
    curve_shape_projection(query_case.geometry, "z", options_.cell_xmin,
                           options_.cell_ymin, options_.cell_size,
                           options_.cell_size, query_zmin, query_zmax);
    const geos::geom::Envelope query_envelope =
        *query_case.geometry->getEnvelopeInternal();

    for (const auto& block : main_blocks_) {
      query_sorted_block(*block, query_case, query_zmin, query_zmax,
                         query_envelope, true, result);
      if (block->live_count > 0 && block->min_zmin > query_zmax) {
        break;
      }
    }
    for (const auto& block : delta_blocks_) {
      query_sorted_block(*block, query_case, query_zmin, query_zmax,
                         query_envelope, false, result);
    }
    query_unsorted_block(open_block_, query_case, query_zmin, query_zmax,
                         query_envelope, result);

    auto end = std::chrono::high_resolution_clock::now();
    result.query_ns = ns_count(end - start);
    return result;
  }

  bool validate_index(std::string* message = nullptr) const {
    auto fail = [&](const std::string& reason) {
      if (message != nullptr) {
        *message = reason;
      }
      return false;
    };
    std::size_t seen_live = 0;
    double previous_min_zmin = -std::numeric_limits<double>::infinity();
    for (const auto& block : main_blocks_) {
      if (!validate_block(*block, true, &seen_live, &previous_min_zmin,
                          message)) {
        return false;
      }
    }
    for (const auto& block : delta_blocks_) {
      if (!validate_block(*block, true, &seen_live, nullptr, message)) {
        return false;
      }
    }
    if (!validate_block(open_block_, false, &seen_live, nullptr, message)) {
      return false;
    }
    if (seen_live != live_count()) {
      return fail("buffered compact overlay live object count mismatch");
    }
    return true;
  }

  std::size_t live_count() const {
    return static_cast<std::size_t>(
        std::count(live_.begin(), live_.end(), static_cast<char>(1)));
  }
  std::size_t block_count() const {
    return main_blocks_.size() + delta_blocks_.size() +
           (open_block_.ids.empty() ? 0 : 1);
  }
  std::size_t stale_block_count() const {
    if (dead_count_ == 0) {
      return 0;
    }
    return block_count();
  }
  std::size_t summary_rebuild_count() const { return summary_rebuild_count_; }
  long long summary_rebuild_ns() const { return summary_rebuild_ns_; }
  std::size_t block_split_count() const {
    return bounded_delta_ ? compaction_count_ : delta_blocks_.size();
  }
  std::size_t adaptive_delta_record_bound() const {
    return adaptive_delta_record_bound_;
  }

  std::size_t index_bytes_estimate() const {
    std::size_t bytes = live_.size() * sizeof(char);
    bytes += (main_blocks_.size() + delta_blocks_.size()) *
             (sizeof(std::unique_ptr<CompactOverlayBlock>) +
              sizeof(CompactOverlayBlock));
    for (const auto& block : main_blocks_) {
      bytes += block->ids.capacity() * sizeof(ObjectId);
    }
    for (const auto& block : delta_blocks_) {
      bytes += block->ids.capacity() * sizeof(ObjectId);
    }
    bytes += open_block_.ids.capacity() * sizeof(ObjectId);
    return bytes;
  }

 private:
  struct ObjectIdLess {
    explicit ObjectIdLess(const std::vector<GeometryMeta>& metadata)
        : metadata(metadata) {}

    bool operator()(ObjectId lhs, ObjectId rhs) const {
      const GeometryMeta& a = metadata[lhs];
      const GeometryMeta& b = metadata[rhs];
      if (a.zmin != b.zmin) {
        return a.zmin < b.zmin;
      }
      return lhs < rhs;
    }

    const std::vector<GeometryMeta>& metadata;
  };

  void expand_summary_on_insert(CompactOverlayBlock& block,
                                const GeometryMeta& meta) {
    if (block.live_count == 0) {
      block.min_zmin = meta.zmin;
      block.max_zmin = meta.zmin;
      block.max_zmax = meta.zmax;
      block.mbr = meta.envelope;
    } else {
      block.min_zmin = std::min(block.min_zmin, meta.zmin);
      block.max_zmin = std::max(block.max_zmin, meta.zmin);
      block.max_zmax = std::max(block.max_zmax, meta.zmax);
      block.mbr.expandToInclude(&meta.envelope);
    }
    ++block.live_count;
  }

  void rebuild_block_summary(CompactOverlayBlock* block) {
    auto start = std::chrono::high_resolution_clock::now();
    bool has_live = false;
    std::size_t live_count = 0;
    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;
    for (ObjectId oid : block->ids) {
      if (oid >= live_.size() || !live_[oid]) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      ++live_count;
      min_zmin = std::min(min_zmin, meta.zmin);
      max_zmin = std::max(max_zmin, meta.zmin);
      max_zmax = std::max(max_zmax, meta.zmax);
      if (!has_live) {
        mbr = meta.envelope;
        has_live = true;
      } else {
        mbr.expandToInclude(&meta.envelope);
      }
    }
    block->live_count = live_count;
    block->dead_count = 0;
    block->stale = false;
    if (has_live) {
      block->min_zmin = min_zmin;
      block->max_zmin = max_zmin;
      block->max_zmax = max_zmax;
      block->mbr = mbr;
    } else {
      block->min_zmin = std::numeric_limits<double>::infinity();
      block->max_zmin = -std::numeric_limits<double>::infinity();
      block->max_zmax = -std::numeric_limits<double>::infinity();
      block->mbr = geos::geom::Envelope();
    }
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void seal_open_block() {
    auto block = std::make_unique<CompactOverlayBlock>();
    block->block_id = open_block_.block_id;
    block->ids.swap(open_block_.ids);
    std::sort(block->ids.begin(), block->ids.end(), ObjectIdLess(metadata_));
    rebuild_block_summary(block.get());
    delta_blocks_.push_back(std::move(block));

    open_block_ = CompactOverlayBlock();
    open_block_.block_id = next_block_id_++;
  }

  std::size_t estimate_adaptive_delta_record_bound() const {
    if (main_blocks_.empty()) {
      return std::max<std::size_t>(options_.block_size, 1);
    }

    const double block_count = static_cast<double>(main_blocks_.size());
    double target_blocks = std::max(std::sqrt(block_count), block_count * 0.08);

    geos::geom::Envelope dataset_mbr;
    bool has_mbr = false;
    std::vector<double> z_spans;
    z_spans.reserve(main_blocks_.size());
    double total_block_area = 0.0;
    std::size_t area_blocks = 0;
    for (const auto& block : main_blocks_) {
      if (block->live_count == 0) {
        continue;
      }
      if (!has_mbr) {
        dataset_mbr = block->mbr;
        has_mbr = true;
      } else {
        dataset_mbr.expandToInclude(&block->mbr);
      }
      const double width = std::max(0.0, block->mbr.getMaxX() - block->mbr.getMinX());
      const double height = std::max(0.0, block->mbr.getMaxY() - block->mbr.getMinY());
      total_block_area += width * height;
      ++area_blocks;
      z_spans.push_back(std::max(0.0, block->max_zmin - block->min_zmin));
    }

    double distribution_factor = 1.0;
    if (has_mbr && area_blocks > 0) {
      const double dataset_width =
          std::max(0.0, dataset_mbr.getMaxX() - dataset_mbr.getMinX());
      const double dataset_height =
          std::max(0.0, dataset_mbr.getMaxY() - dataset_mbr.getMinY());
      const double dataset_area = dataset_width * dataset_height;
      if (dataset_area > 0.0) {
        const double avg_area_ratio =
            (total_block_area / static_cast<double>(area_blocks)) / dataset_area;
        if (avg_area_ratio > 0.02) {
          distribution_factor *= 0.60;
        } else if (avg_area_ratio < 0.002) {
          distribution_factor *= 1.25;
        }
      }
    }

    if (z_spans.size() > 1) {
      const double mean =
          std::accumulate(z_spans.begin(), z_spans.end(), 0.0) /
          static_cast<double>(z_spans.size());
      if (mean > 0.0) {
        double variance = 0.0;
        for (double span : z_spans) {
          const double diff = span - mean;
          variance += diff * diff;
        }
        variance /= static_cast<double>(z_spans.size());
        const double cv = std::sqrt(variance) / mean;
        if (cv > 2.0) {
          distribution_factor *= 0.70;
        } else if (cv < 0.75) {
          distribution_factor *= 1.15;
        }
      }
    }

    target_blocks *= distribution_factor;
    const std::size_t min_blocks = 16;
    const std::size_t max_blocks = 256;
    std::size_t bounded_blocks = static_cast<std::size_t>(std::ceil(target_blocks));
    bounded_blocks = std::max(min_blocks, std::min(max_blocks, bounded_blocks));
    return std::max<std::size_t>(options_.block_size,
                                 bounded_blocks * options_.block_size);
  }

  void promote_delta_blocks_to_main() {
    if (!open_block_.ids.empty()) {
      seal_open_block();
    }
    if (delta_blocks_.empty()) {
      delta_record_count_ = 0;
      return;
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<ObjectId> ids;
    ids.reserve(live_count());
    for (const auto& block : main_blocks_) {
      for (ObjectId oid : block->ids) {
        if (oid < live_.size() && live_[oid]) {
          ids.push_back(oid);
        }
      }
    }
    for (const auto& block : delta_blocks_) {
      for (ObjectId oid : block->ids) {
        if (oid < live_.size() && live_[oid]) {
          ids.push_back(oid);
        }
      }
    }
    std::sort(ids.begin(), ids.end(), ObjectIdLess(metadata_));

    main_blocks_.clear();
    delta_blocks_.clear();
    main_blocks_.reserve((ids.size() + options_.block_size - 1) /
                         options_.block_size);
    for (std::size_t begin = 0; begin < ids.size();
         begin += options_.block_size) {
      std::size_t end = std::min(begin + options_.block_size, ids.size());
      std::unique_ptr<CompactOverlayBlock> block(new CompactOverlayBlock());
      block->block_id = next_block_id_++;
      block->ids.assign(ids.begin() + begin, ids.begin() + end);
      rebuild_block_summary(block.get());
      main_blocks_.push_back(std::move(block));
    }
    delta_record_count_ = 0;
    ++compaction_count_;
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void query_sorted_block(const CompactOverlayBlock& block,
                          const QueryCase& query_case, double query_zmin,
                          double query_zmax,
                          const geos::geom::Envelope& query_envelope,
                          bool allow_order_break,
                          SimpleQueryResult& result) const {
    if (block.live_count == 0) {
      return;
    }
    if (allow_order_break && block.min_zmin > query_zmax) {
      return;
    }
    if (block.max_zmax < query_zmin) {
      return;
    }
    if (!envelopes_intersect(block.mbr, query_envelope)) {
      return;
    }
    for (ObjectId oid : block.ids) {
      if (oid >= metadata_.size()) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      if (meta.zmin > query_zmax) {
        break;
      }
      if (!live_[oid]) {
        continue;
      }
      if (meta.zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(meta.envelope, query_envelope)) {
        continue;
      }
      ++result.candidates;
      ++result.exact_calls;
      if (query_case.geometry->intersects(geometries_[oid].get())) {
        result.answers.insert(oid);
      }
    }
  }

  void query_unsorted_block(const CompactOverlayBlock& block,
                            const QueryCase& query_case, double query_zmin,
                            double query_zmax,
                            const geos::geom::Envelope& query_envelope,
                            SimpleQueryResult& result) const {
    if (block.live_count == 0 || block.max_zmax < query_zmin ||
        !envelopes_intersect(block.mbr, query_envelope)) {
      return;
    }
    for (ObjectId oid : block.ids) {
      if (oid >= metadata_.size() || !live_[oid]) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      if (meta.zmin > query_zmax || meta.zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(meta.envelope, query_envelope)) {
        continue;
      }
      ++result.candidates;
      ++result.exact_calls;
      if (query_case.geometry->intersects(geometries_[oid].get())) {
        result.answers.insert(oid);
      }
    }
  }

  bool validate_block(const CompactOverlayBlock& block, bool require_sorted,
                      std::size_t* seen_live, double* previous_min_zmin,
                      std::string* message) const {
    auto fail = [&](const std::string& reason) {
      if (message != nullptr) {
        *message = reason;
      }
      return false;
    };
    if (require_sorted) {
      for (std::size_t i = 1; i < block.ids.size(); ++i) {
        if (ObjectIdLess(metadata_)(block.ids[i], block.ids[i - 1])) {
          return fail("buffered compact overlay block ids are not sorted");
        }
      }
    }

    bool has_live = false;
    double true_min_zmin = std::numeric_limits<double>::infinity();
    double true_max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope true_mbr;
    for (ObjectId oid : block.ids) {
      if (oid >= live_.size()) {
        return fail("buffered compact overlay object id out of range");
      }
      if (!live_[oid]) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      ++(*seen_live);
      true_min_zmin = std::min(true_min_zmin, meta.zmin);
      true_max_zmax = std::max(true_max_zmax, meta.zmax);
      if (!has_live) {
        true_mbr = meta.envelope;
        has_live = true;
      } else {
        true_mbr.expandToInclude(&meta.envelope);
      }
    }
    if (!has_live) {
      return true;
    }
    if (require_sorted && previous_min_zmin != nullptr &&
        block.min_zmin + 1e-9 < *previous_min_zmin) {
      return fail("buffered compact overlay main blocks are not ordered");
    }
    if (require_sorted && previous_min_zmin != nullptr) {
      *previous_min_zmin = block.min_zmin;
    }
    const double eps = 1e-9;
    if (block.max_zmax + eps < true_max_zmax) {
      return fail("buffered compact overlay max_zmax is not conservative");
    }
    if (block.min_zmin - eps > true_min_zmin) {
      return fail("buffered compact overlay min_zmin is not conservative");
    }
    if (!envelope_contains(block.mbr, true_mbr)) {
      return fail("buffered compact overlay MBR is not conservative");
    }
    return true;
  }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  bool bounded_delta_ = false;
  std::vector<char> live_;
  std::vector<std::unique_ptr<CompactOverlayBlock>> main_blocks_;
  std::vector<std::unique_ptr<CompactOverlayBlock>> delta_blocks_;
  CompactOverlayBlock open_block_;
  std::uint64_t next_block_id_ = 0;
  std::size_t dead_count_ = 0;
  std::size_t delta_record_count_ = 0;
  std::size_t adaptive_delta_record_bound_ = 0;
  std::size_t compaction_count_ = 0;
  mutable std::size_t summary_rebuild_count_ = 0;
  mutable long long summary_rebuild_ns_ = 0;
};

class DeliAlexHybridBufferedIndex {
 public:
  using BaseAlex = alex::Alex<double, Geometry*>;
  using GlinIndex = alex::Glin<double, Geometry*>;

  DeliAlexHybridBufferedIndex(const Options& options,
                              const std::vector<GeometryPtr>& geometries,
                              const std::vector<GeometryMeta>& metadata,
                              bool bounded_delta = false)
      : options_(options),
        geometries_(geometries),
        metadata_(metadata),
        overlay_(options, geometries, metadata, bounded_delta) {}

  void bulk_load(const std::vector<ObjectId>& ids) {
    std::vector<std::pair<double, Geometry*>> values;
    values.reserve(ids.size());
    for (ObjectId oid : ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      values.emplace_back(metadata_[oid].zmin, geometries_[oid].get());
    }
    std::sort(values.begin(), values.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                  return lhs.first < rhs.first;
                }
                return lhs.second < rhs.second;
              });
    base().bulk_load(values.data(), static_cast<int>(values.size()));
    overlay_.bulk_load(ids);
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    auto result = base().insert(metadata_[oid].zmin, geometries_[oid].get());
    if (!result.second) {
      return false;
    }
    return overlay_.insert(oid);
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    int erased = base().erase_geo(metadata_[oid].zmin, geometries_[oid].get());
    if (erased <= 0) {
      return false;
    }
    return overlay_.erase(oid);
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    return overlay_.query(query_case);
  }

  bool validate_index(std::string* message = nullptr) const {
    return overlay_.validate_index(message);
  }

  std::size_t block_count() const { return overlay_.block_count(); }
  std::size_t stale_block_count() const { return overlay_.stale_block_count(); }
  std::size_t summary_rebuild_count() const {
    return overlay_.summary_rebuild_count();
  }
  long long summary_rebuild_ns() const { return overlay_.summary_rebuild_ns(); }
  std::size_t block_split_count() const { return overlay_.block_split_count(); }
  std::size_t adaptive_delta_record_bound() const {
    return overlay_.adaptive_delta_record_bound();
  }
  std::size_t leaf_count() const {
    return static_cast<std::size_t>(std::max(0, base().num_leaves()));
  }
  std::size_t index_bytes_estimate() const {
    return static_cast<std::size_t>(
               std::max<long long>(0, base().data_size() + base().model_size())) +
           overlay_.index_bytes_estimate();
  }

 private:
  BaseAlex& base() { return static_cast<BaseAlex&>(index_); }
  const BaseAlex& base() const { return static_cast<const BaseAlex&>(index_); }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  GlinIndex index_;
  BufferedCompactQueryOverlay overlay_;
};

struct LocalBoundedOverlayBlock {
  std::uint64_t block_id = 0;
  std::vector<ObjectId> compact_ids;
  std::vector<ObjectId> delta_ids;
  double min_zmin = std::numeric_limits<double>::infinity();
  double max_zmin = -std::numeric_limits<double>::infinity();
  double max_zmax = -std::numeric_limits<double>::infinity();
  geos::geom::Envelope mbr;
  std::size_t live_count = 0;
  std::size_t live_delta_count = 0;
  std::size_t dead_count = 0;
  bool stale = false;
};

class LocalBoundedCompactQueryOverlay {
 public:
  LocalBoundedCompactQueryOverlay(const Options& options,
                                  const std::vector<GeometryPtr>& geometries,
                                  const std::vector<GeometryMeta>& metadata)
      : options_(options), geometries_(geometries), metadata_(metadata) {
    live_.assign(geometries.size(), 0);
    in_delta_.assign(geometries.size(), 0);
    object_to_block_.assign(geometries.size(), nullptr);
    local_delta_bound_ = options_.local_delta_bound == 0
                             ? std::max<std::size_t>(64, options_.block_size / 4)
                             : options_.local_delta_bound;
  }

  void bulk_load(const std::vector<ObjectId>& object_ids) {
    std::vector<ObjectId> ids;
    ids.reserve(object_ids.size());
    for (ObjectId oid : object_ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      ids.push_back(oid);
      live_[oid] = 1;
    }
    std::sort(ids.begin(), ids.end(), ObjectIdLess(metadata_));

    for (std::size_t begin = 0; begin < ids.size();
         begin += options_.block_size) {
      const std::size_t end = std::min(begin + options_.block_size, ids.size());
      std::unique_ptr<LocalBoundedOverlayBlock> block(
          new LocalBoundedOverlayBlock());
      block->block_id = next_block_id_++;
      block->compact_ids.assign(ids.begin() + begin, ids.begin() + end);
      LocalBoundedOverlayBlock* block_ptr = block.get();
      owned_blocks_.push_back(std::move(block));
      directory_.push_back(block_ptr);
      rebuild_block_summary(block_ptr, false);
    }
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size() || live_[oid]) {
      return false;
    }
    LocalBoundedOverlayBlock* block = find_block_for_zmin(metadata_[oid].zmin);
    if (block == nullptr) {
      block = create_empty_block();
    }
    block->delta_ids.push_back(oid);
    live_[oid] = 1;
    in_delta_[oid] = 1;
    object_to_block_[oid] = block;
    expand_summary_on_insert(block, metadata_[oid]);
    ++block->live_delta_count;
    if (block->live_delta_count >= local_delta_bound_) {
      compact_block(block);
    }
    return true;
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size() || !live_[oid]) {
      return false;
    }
    LocalBoundedOverlayBlock* block = object_to_block_[oid];
    const GeometryMeta& meta = metadata_[oid];
    const bool summary_critical =
        block != nullptr && is_summary_critical(*block, meta);
    live_[oid] = 0;
    object_to_block_[oid] = nullptr;
    if (oid < in_delta_.size() && in_delta_[oid]) {
      in_delta_[oid] = 0;
      if (block != nullptr && block->live_delta_count > 0) {
        --block->live_delta_count;
      }
    }
    if (block != nullptr) {
      if (block->live_count > 0) {
        --block->live_count;
      }
      ++block->dead_count;
      block->stale = true;
      if (should_compact_after_delete(*block)) {
        compact_block(block);
      } else if (summary_critical) {
        refresh_block_summary(block);
      }
    }
    return true;
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    auto start = std::chrono::high_resolution_clock::now();
    SimpleQueryResult result;

    double query_zmin = 0.0;
    double query_zmax = 0.0;
    curve_shape_projection(query_case.geometry, "z", options_.cell_xmin,
                           options_.cell_ymin, options_.cell_size,
                           options_.cell_size, query_zmin, query_zmax);
    const geos::geom::Envelope query_envelope =
        *query_case.geometry->getEnvelopeInternal();

    for (const LocalBoundedOverlayBlock* block : directory_) {
      if (block->live_count == 0) {
        continue;
      }
      if (block->min_zmin > query_zmax) {
        break;
      }
      if (block->max_zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(block->mbr, query_envelope)) {
        continue;
      }
      query_compact_ids(*block, query_case, query_zmin, query_zmax,
                        query_envelope, result);
      query_delta_ids(*block, query_case, query_zmin, query_zmax,
                      query_envelope, result);
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.query_ns = ns_count(end - start);
    return result;
  }

  bool validate_index(std::string* message = nullptr) const {
    auto fail = [&](const std::string& reason) {
      if (message != nullptr) {
        *message = reason;
      }
      return false;
    };

    std::size_t seen_live = 0;
    bool have_previous_live_block = false;
    double previous_live_min_zmin = -std::numeric_limits<double>::infinity();
    for (const LocalBoundedOverlayBlock* block : directory_) {
      for (std::size_t i = 1; i < block->compact_ids.size(); ++i) {
        if (ObjectIdLess(metadata_)(block->compact_ids[i],
                                    block->compact_ids[i - 1])) {
          return fail("local bounded compact ids are not sorted by zmin");
        }
      }

      bool has_live = false;
      std::size_t true_live = 0;
      std::size_t true_delta_live = 0;
      double true_min_zmin = std::numeric_limits<double>::infinity();
      double true_max_zmax = -std::numeric_limits<double>::infinity();
      geos::geom::Envelope true_mbr;

      auto visit_id = [&](ObjectId oid, bool expect_delta) -> bool {
        if (oid >= live_.size()) {
          return fail("local bounded overlay object id out of range");
        }
        if (!live_[oid]) {
          return true;
        }
        if (object_to_block_[oid] != block) {
          return fail("local bounded object points to a different block");
        }
        if (expect_delta && !in_delta_[oid]) {
          return fail("local bounded delta object is not marked as delta");
        }
        if (!expect_delta && in_delta_[oid]) {
          return fail("local bounded compact object is marked as delta");
        }
        const GeometryMeta& meta = metadata_[oid];
        ++true_live;
        ++seen_live;
        if (expect_delta) {
          ++true_delta_live;
        }
        true_min_zmin = std::min(true_min_zmin, meta.zmin);
        true_max_zmax = std::max(true_max_zmax, meta.zmax);
        if (!has_live) {
          true_mbr = meta.envelope;
          has_live = true;
        } else {
          true_mbr.expandToInclude(&meta.envelope);
        }
        return true;
      };

      for (ObjectId oid : block->compact_ids) {
        if (!visit_id(oid, false)) {
          return false;
        }
      }
      for (ObjectId oid : block->delta_ids) {
        if (!visit_id(oid, true)) {
          return false;
        }
      }

      if (block->live_count != true_live) {
        return fail("local bounded live_count mismatch");
      }
      if (block->live_delta_count != true_delta_live) {
        return fail("local bounded live_delta_count mismatch");
      }
      if (true_live == 0) {
        continue;
      }
      if (have_previous_live_block &&
          block->min_zmin + 1e-9 < previous_live_min_zmin) {
        return fail("local bounded directory is not sorted by min_zmin");
      }
      previous_live_min_zmin = block->min_zmin;
      have_previous_live_block = true;
      const double eps = 1e-9;
      if (block->max_zmax + eps < true_max_zmax) {
        return fail("local bounded max_zmax is not conservative");
      }
      if (block->min_zmin - eps > true_min_zmin) {
        return fail("local bounded min_zmin is not conservative");
      }
      if (!envelope_contains(block->mbr, true_mbr)) {
        return fail("local bounded MBR is not conservative");
      }
    }

    if (seen_live != live_count()) {
      return fail("local bounded overlay live object count mismatch");
    }
    return true;
  }

  std::size_t live_count() const {
    return static_cast<std::size_t>(
        std::count(live_.begin(), live_.end(), static_cast<char>(1)));
  }
  std::size_t block_count() const { return directory_.size(); }
  std::size_t stale_block_count() const {
    std::size_t count = 0;
    for (const LocalBoundedOverlayBlock* block : directory_) {
      if (block->stale || block->live_delta_count > 0) {
        ++count;
      }
    }
    return count;
  }
  std::size_t summary_rebuild_count() const { return summary_rebuild_count_; }
  long long summary_rebuild_ns() const { return summary_rebuild_ns_; }
  std::size_t block_split_count() const { return block_split_count_; }
  std::size_t local_compaction_count() const { return local_compaction_count_; }
  long long local_compaction_ns() const { return local_compaction_ns_; }
  std::size_t local_delta_bound() const { return local_delta_bound_; }
  std::size_t delete_compact_bound() const { return delete_compact_bound_; }
  std::size_t max_local_delta_size() const {
    std::size_t value = 0;
    for (const LocalBoundedOverlayBlock* block : directory_) {
      value = std::max(value, block->live_delta_count);
    }
    return value;
  }
  double avg_local_delta_size() const {
    if (directory_.empty()) {
      return 0.0;
    }
    std::size_t total = 0;
    for (const LocalBoundedOverlayBlock* block : directory_) {
      total += block->live_delta_count;
    }
    return static_cast<double>(total) /
           static_cast<double>(directory_.size());
  }
  std::size_t blocks_with_delta() const {
    std::size_t count = 0;
    for (const LocalBoundedOverlayBlock* block : directory_) {
      if (block->live_delta_count > 0) {
        ++count;
      }
    }
    return count;
  }
  double tombstone_ratio() const {
    std::size_t live = 0;
    std::size_t dead = 0;
    for (const LocalBoundedOverlayBlock* block : directory_) {
      live += block->live_count;
      dead += block->dead_count;
    }
    const std::size_t denom = live + dead;
    return denom == 0 ? 0.0 : static_cast<double>(dead) /
                                static_cast<double>(denom);
  }

  std::size_t index_bytes_estimate() const {
    std::size_t bytes = live_.size() * sizeof(char);
    bytes += in_delta_.size() * sizeof(char);
    bytes += object_to_block_.size() * sizeof(LocalBoundedOverlayBlock*);
    bytes += directory_.size() * sizeof(LocalBoundedOverlayBlock*);
    bytes += owned_blocks_.size() * sizeof(LocalBoundedOverlayBlock);
    for (const auto& block : owned_blocks_) {
      bytes += block->compact_ids.capacity() * sizeof(ObjectId);
      bytes += block->delta_ids.capacity() * sizeof(ObjectId);
    }
    return bytes;
  }

 private:
  struct ObjectIdLess {
    explicit ObjectIdLess(const std::vector<GeometryMeta>& metadata)
        : metadata(metadata) {}

    bool operator()(ObjectId lhs, ObjectId rhs) const {
      const GeometryMeta& a = metadata[lhs];
      const GeometryMeta& b = metadata[rhs];
      if (a.zmin != b.zmin) {
        return a.zmin < b.zmin;
      }
      return lhs < rhs;
    }

    const std::vector<GeometryMeta>& metadata;
  };

  LocalBoundedOverlayBlock* create_empty_block() {
    std::unique_ptr<LocalBoundedOverlayBlock> block(
        new LocalBoundedOverlayBlock());
    block->block_id = next_block_id_++;
    LocalBoundedOverlayBlock* block_ptr = block.get();
    owned_blocks_.push_back(std::move(block));
    directory_.push_back(block_ptr);
    return block_ptr;
  }

  LocalBoundedOverlayBlock* find_block_for_zmin(double zmin) const {
    if (directory_.empty()) {
      return nullptr;
    }
    auto it = std::lower_bound(
        directory_.begin(), directory_.end(), zmin,
        [](const LocalBoundedOverlayBlock* block, double value) {
          return block->max_zmin < value;
        });
    if (it == directory_.end()) {
      return directory_.back();
    }
    return *it;
  }

  void expand_summary_on_insert(LocalBoundedOverlayBlock* block,
                                const GeometryMeta& meta) {
    if (block->live_count == 0) {
      block->min_zmin = meta.zmin;
      block->max_zmin = meta.zmin;
      block->max_zmax = meta.zmax;
      block->mbr = meta.envelope;
    } else {
      block->min_zmin = std::min(block->min_zmin, meta.zmin);
      block->max_zmin = std::max(block->max_zmin, meta.zmin);
      block->max_zmax = std::max(block->max_zmax, meta.zmax);
      block->mbr.expandToInclude(&meta.envelope);
    }
    ++block->live_count;
  }

  std::size_t delete_compact_threshold_count() const {
    if (options_.delete_compact_fraction <= 0.0) {
      return 0;
    }
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(options_.delete_compact_fraction *
                         static_cast<double>(options_.block_size))));
  }

  bool should_compact_after_delete(const LocalBoundedOverlayBlock& block) const {
    const std::size_t threshold = delete_compact_bound_;
    if (threshold == 0) {
      return true;
    }
    return block.dead_count >= threshold;
  }

  bool is_summary_critical(const LocalBoundedOverlayBlock& block,
                           const GeometryMeta& meta) const {
    if (block.live_count <= 1) {
      return true;
    }
    const double eps = 1e-9;
    if (meta.zmin <= block.min_zmin + eps ||
        meta.zmin >= block.max_zmin - eps ||
        meta.zmax >= block.max_zmax - eps) {
      return true;
    }
    return meta.envelope.getMinX() <= block.mbr.getMinX() + eps ||
           meta.envelope.getMaxX() >= block.mbr.getMaxX() - eps ||
           meta.envelope.getMinY() <= block.mbr.getMinY() + eps ||
           meta.envelope.getMaxY() >= block.mbr.getMaxY() - eps;
  }

  void rebuild_block_summary(LocalBoundedOverlayBlock* block,
                             bool compact_dead) {
    bool has_live = false;
    std::size_t live_count = 0;
    std::size_t live_delta_count = 0;
    std::size_t dead_count = 0;
    std::vector<ObjectId> live_compact_ids;
    std::vector<ObjectId> live_delta_ids;
    if (compact_dead) {
      live_compact_ids.reserve(block->compact_ids.size());
      live_delta_ids.reserve(block->delta_ids.size());
    }

    double min_zmin = std::numeric_limits<double>::infinity();
    double max_zmin = -std::numeric_limits<double>::infinity();
    double max_zmax = -std::numeric_limits<double>::infinity();
    geos::geom::Envelope mbr;

    auto visit = [&](ObjectId oid, bool is_delta) {
      if (oid >= live_.size()) {
        if (!compact_dead) {
          ++dead_count;
        }
        return;
      }
      if (!live_[oid]) {
        if (!compact_dead) {
          ++dead_count;
        }
        return;
      }
      if (compact_dead) {
        if (is_delta) {
          live_delta_ids.push_back(oid);
        } else {
          live_compact_ids.push_back(oid);
        }
      } else if (is_delta) {
        ++live_delta_count;
      }
      object_to_block_[oid] = block;
      const GeometryMeta& meta = metadata_[oid];
      ++live_count;
      min_zmin = std::min(min_zmin, meta.zmin);
      max_zmin = std::max(max_zmin, meta.zmin);
      max_zmax = std::max(max_zmax, meta.zmax);
      if (!has_live) {
        mbr = meta.envelope;
        has_live = true;
      } else {
        mbr.expandToInclude(&meta.envelope);
      }
    };

    for (ObjectId oid : block->compact_ids) {
      visit(oid, false);
    }
    for (ObjectId oid : block->delta_ids) {
      visit(oid, true);
    }

    if (compact_dead) {
      ObjectIdLess less(metadata_);
      std::sort(live_delta_ids.begin(), live_delta_ids.end(), less);
      std::vector<ObjectId> merged_ids;
      merged_ids.reserve(live_compact_ids.size() + live_delta_ids.size());
      std::merge(live_compact_ids.begin(), live_compact_ids.end(),
                 live_delta_ids.begin(), live_delta_ids.end(),
                 std::back_inserter(merged_ids), less);
      for (ObjectId oid : merged_ids) {
        in_delta_[oid] = 0;
        object_to_block_[oid] = block;
      }
      block->compact_ids.swap(merged_ids);
      block->delta_ids.clear();
      live_delta_count = 0;
    }
    block->live_count = live_count;
    block->live_delta_count = live_delta_count;
    block->dead_count = compact_dead ? 0 : dead_count;
    block->stale = block->dead_count > 0;
    if (has_live) {
      block->min_zmin = min_zmin;
      block->max_zmin = max_zmin;
      block->max_zmax = max_zmax;
      block->mbr = mbr;
    } else {
      block->min_zmin = std::numeric_limits<double>::infinity();
      block->max_zmin = -std::numeric_limits<double>::infinity();
      block->max_zmax = -std::numeric_limits<double>::infinity();
      block->mbr = geos::geom::Envelope();
    }
  }

  void compact_block(LocalBoundedOverlayBlock* block) {
    auto start = std::chrono::high_resolution_clock::now();
    rebuild_block_summary(block, true);
    if (block->compact_ids.size() > 2 * options_.block_size) {
      split_block(block);
    }
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    ++local_compaction_count_;
    const long long elapsed_ns = ns_count(end - start);
    summary_rebuild_ns_ += elapsed_ns;
    local_compaction_ns_ += elapsed_ns;
  }

  void refresh_block_summary(LocalBoundedOverlayBlock* block) {
    auto start = std::chrono::high_resolution_clock::now();
    const double previous_min_zmin = block->min_zmin;
    rebuild_block_summary(block, false);
    if (block->live_count > 0 &&
        previous_min_zmin != std::numeric_limits<double>::infinity()) {
      block->min_zmin = std::min(block->min_zmin, previous_min_zmin);
    }
    auto end = std::chrono::high_resolution_clock::now();
    ++summary_rebuild_count_;
    summary_rebuild_ns_ += ns_count(end - start);
  }

  void split_block(LocalBoundedOverlayBlock* block) {
    std::sort(block->compact_ids.begin(), block->compact_ids.end(),
              ObjectIdLess(metadata_));
    const std::size_t mid = block->compact_ids.size() / 2;
    std::unique_ptr<LocalBoundedOverlayBlock> right(
        new LocalBoundedOverlayBlock());
    right->block_id = next_block_id_++;
    right->compact_ids.assign(block->compact_ids.begin() + mid,
                              block->compact_ids.end());
    block->compact_ids.erase(block->compact_ids.begin() + mid,
                             block->compact_ids.end());

    LocalBoundedOverlayBlock* right_ptr = right.get();
    owned_blocks_.push_back(std::move(right));

    rebuild_block_summary(block, false);
    rebuild_block_summary(right_ptr, false);

    auto it = std::find(directory_.begin(), directory_.end(), block);
    if (it == directory_.end()) {
      directory_.push_back(right_ptr);
    } else {
      directory_.insert(it + 1, right_ptr);
    }
    ++block_split_count_;
  }

  void query_compact_ids(const LocalBoundedOverlayBlock& block,
                         const QueryCase& query_case, double query_zmin,
                         double query_zmax,
                         const geos::geom::Envelope& query_envelope,
                         SimpleQueryResult& result) const {
    for (ObjectId oid : block.compact_ids) {
      if (oid >= metadata_.size()) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      if (meta.zmin > query_zmax) {
        break;
      }
      if (!live_[oid]) {
        continue;
      }
      if (meta.zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(meta.envelope, query_envelope)) {
        continue;
      }
      ++result.candidates;
      ++result.exact_calls;
      if (query_case.geometry->intersects(geometries_[oid].get())) {
        result.answers.insert(oid);
      }
    }
  }

  void query_delta_ids(const LocalBoundedOverlayBlock& block,
                       const QueryCase& query_case, double query_zmin,
                       double query_zmax,
                       const geos::geom::Envelope& query_envelope,
                       SimpleQueryResult& result) const {
    for (ObjectId oid : block.delta_ids) {
      if (oid >= metadata_.size() || !live_[oid]) {
        continue;
      }
      const GeometryMeta& meta = metadata_[oid];
      if (meta.zmin > query_zmax || meta.zmax < query_zmin) {
        continue;
      }
      if (!envelopes_intersect(meta.envelope, query_envelope)) {
        continue;
      }
      ++result.candidates;
      ++result.exact_calls;
      if (query_case.geometry->intersects(geometries_[oid].get())) {
        result.answers.insert(oid);
      }
    }
  }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  std::vector<char> live_;
  std::vector<char> in_delta_;
  std::vector<LocalBoundedOverlayBlock*> object_to_block_;
  std::vector<std::unique_ptr<LocalBoundedOverlayBlock>> owned_blocks_;
  std::vector<LocalBoundedOverlayBlock*> directory_;
  std::uint64_t next_block_id_ = 0;
  std::size_t local_delta_bound_ = 0;
  std::size_t delete_compact_bound_ = delete_compact_threshold_count();
  std::size_t summary_rebuild_count_ = 0;
  long long summary_rebuild_ns_ = 0;
  std::size_t local_compaction_count_ = 0;
  long long local_compaction_ns_ = 0;
  std::size_t block_split_count_ = 0;
};

class DeliAlexHybridLocalBoundedIndex {
 public:
  using BaseAlex = alex::Alex<double, Geometry*>;
  using GlinIndex = alex::Glin<double, Geometry*>;

  DeliAlexHybridLocalBoundedIndex(
      const Options& options, const std::vector<GeometryPtr>& geometries,
      const std::vector<GeometryMeta>& metadata)
      : options_(options),
        geometries_(geometries),
        metadata_(metadata),
        overlay_(options, geometries, metadata) {}

  void bulk_load(const std::vector<ObjectId>& ids) {
    std::vector<std::pair<double, Geometry*>> values;
    values.reserve(ids.size());
    for (ObjectId oid : ids) {
      if (oid >= geometries_.size()) {
        continue;
      }
      values.emplace_back(metadata_[oid].zmin, geometries_[oid].get());
    }
    std::sort(values.begin(), values.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                  return lhs.first < rhs.first;
                }
                return lhs.second < rhs.second;
              });
    base().bulk_load(values.data(), static_cast<int>(values.size()));
    overlay_.bulk_load(ids);
  }

  bool insert(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    auto result = base().insert(metadata_[oid].zmin, geometries_[oid].get());
    if (!result.second) {
      return false;
    }
    return overlay_.insert(oid);
  }

  bool erase(ObjectId oid) {
    if (oid >= geometries_.size()) {
      return false;
    }
    int erased = base().erase_geo(metadata_[oid].zmin, geometries_[oid].get());
    if (erased <= 0) {
      return false;
    }
    return overlay_.erase(oid);
  }

  SimpleQueryResult query(const QueryCase& query_case) const {
    return overlay_.query(query_case);
  }

  bool validate_index(std::string* message = nullptr) const {
    return overlay_.validate_index(message);
  }

  std::size_t block_count() const { return overlay_.block_count(); }
  std::size_t stale_block_count() const { return overlay_.stale_block_count(); }
  std::size_t summary_rebuild_count() const {
    return overlay_.summary_rebuild_count();
  }
  long long summary_rebuild_ns() const { return overlay_.summary_rebuild_ns(); }
  std::size_t block_split_count() const { return overlay_.block_split_count(); }
  std::size_t local_compaction_count() const {
    return overlay_.local_compaction_count();
  }
  long long local_compaction_ns() const {
    return overlay_.local_compaction_ns();
  }
  std::size_t local_delta_bound() const { return overlay_.local_delta_bound(); }
  std::size_t delete_compact_bound() const {
    return overlay_.delete_compact_bound();
  }
  std::size_t max_local_delta_size() const {
    return overlay_.max_local_delta_size();
  }
  double avg_local_delta_size() const {
    return overlay_.avg_local_delta_size();
  }
  std::size_t blocks_with_delta() const { return overlay_.blocks_with_delta(); }
  double tombstone_ratio() const { return overlay_.tombstone_ratio(); }
  std::size_t leaf_count() const {
    return static_cast<std::size_t>(std::max(0, base().num_leaves()));
  }
  std::size_t index_bytes_estimate() const {
    return static_cast<std::size_t>(
               std::max<long long>(0, base().data_size() + base().model_size())) +
           overlay_.index_bytes_estimate();
  }

 private:
  BaseAlex& base() { return static_cast<BaseAlex&>(index_); }
  const BaseAlex& base() const { return static_cast<const BaseAlex&>(index_); }

  const Options& options_;
  const std::vector<GeometryPtr>& geometries_;
  const std::vector<GeometryMeta>& metadata_;
  GlinIndex index_;
  LocalBoundedCompactQueryOverlay overlay_;
};

std::unordered_map<Geometry*, ObjectId> build_geometry_to_oid(
    const std::vector<GeometryPtr>& geometries) {
  std::unordered_map<Geometry*, ObjectId> map;
  map.reserve(geometries.size());
  for (ObjectId oid = 0; oid < geometries.size(); ++oid) {
    map[geometries[oid].get()] = oid;
  }
  return map;
}

std::vector<GeometryMeta> build_geometry_metadata(
    const std::vector<GeometryPtr>& geometries, const Options& options) {
  std::vector<GeometryMeta> metadata;
  metadata.reserve(geometries.size());
  for (ObjectId oid = 0; oid < geometries.size(); ++oid) {
    GeometryMeta meta;
    meta.object_id = oid;
    meta.envelope = *geometries[oid]->getEnvelopeInternal();
    curve_shape_projection(geometries[oid].get(), "z", options.cell_xmin,
                           options.cell_ymin, options.cell_size,
                           options.cell_size, meta.zmin, meta.zmax);
    metadata.push_back(meta);
  }
  return metadata;
}

RTree build_rtree_for_ids(const std::vector<GeometryPtr>& geometries,
                          const std::vector<ObjectId>& ids,
                          const std::vector<char>& live,
                          long long* build_ns) {
  auto start = std::chrono::high_resolution_clock::now();
  std::vector<RTreeValue> values;
  values.reserve(ids.size());
  for (ObjectId oid : ids) {
    if (oid < live.size() && live[oid]) {
      values.emplace_back(
          boost_box_from_envelope(*geometries[oid]->getEnvelopeInternal()),
          oid);
    }
  }
  RTree rtree(values.begin(), values.end());
  auto end = std::chrono::high_resolution_clock::now();
  if (build_ns != nullptr) {
    *build_ns = ns_count(end - start);
  }
  return rtree;
}

SimpleQueryResult query_boost_index(const RTree& rtree,
                                    const std::vector<GeometryPtr>& geometries,
                                    const std::vector<char>& live,
                                    const QueryCase& query_case) {
  auto start = std::chrono::high_resolution_clock::now();
  SimpleQueryResult result;
  std::vector<RTreeValue> hits;
  const geos::geom::Envelope query_envelope =
      *query_case.geometry->getEnvelopeInternal();
  rtree.query(bgi::intersects(boost_box_from_envelope(query_envelope)),
              std::back_inserter(hits));
  result.candidates = hits.size();
  for (const auto& hit : hits) {
    ObjectId oid = hit.second;
    if (oid >= geometries.size() || oid >= live.size() || !live[oid]) {
      continue;
    }
    ++result.exact_calls;
    if (query_case.geometry->intersects(geometries[oid].get())) {
      result.answers.insert(oid);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  result.query_ns = ns_count(end - start);
  return result;
}

SimpleQueryResult query_quadtree_index(
    Quadtree& quadtree,
    const std::unordered_map<Geometry*, ObjectId>& geometry_to_oid,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<char>& live,
    const QueryCase& query_case) {
  auto start = std::chrono::high_resolution_clock::now();
  SimpleQueryResult result;
  std::vector<void*> hits;
  const geos::geom::Envelope query_envelope =
      *query_case.geometry->getEnvelopeInternal();
  quadtree.query(&query_envelope, hits);
  result.candidates = hits.size();
  for (void* item : hits) {
    Geometry* geometry = static_cast<Geometry*>(item);
    auto found = geometry_to_oid.find(geometry);
    if (found == geometry_to_oid.end()) {
      continue;
    }
    ObjectId oid = found->second;
    if (oid >= live.size() || !live[oid]) {
      continue;
    }
    ++result.exact_calls;
    if (query_case.geometry->intersects(geometries[oid].get())) {
      result.answers.insert(oid);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  result.query_ns = ns_count(end - start);
  return result;
}

SimpleQueryResult query_glin_piece_index(
    alex::Glin<double, Geometry*>& index,
    std::vector<std::tuple<double, double, double, double>>& pieces,
    const std::unordered_map<Geometry*, ObjectId>& geometry_to_oid,
    const std::vector<char>& live,
    const Options& options,
    const QueryCase& query_case) {
  auto start = std::chrono::high_resolution_clock::now();
  SimpleQueryResult result;
  std::vector<Geometry*> found_geometries;
  int count_filter = 0;
  index.glin_find(query_case.geometry, "z", options.cell_xmin,
                  options.cell_ymin, options.cell_size, options.cell_size,
                  pieces, found_geometries, count_filter);
  result.candidates = static_cast<std::size_t>(std::max(0, count_filter));
  result.exact_calls = result.candidates;
  for (Geometry* geometry : found_geometries) {
    auto found = geometry_to_oid.find(geometry);
    if (found == geometry_to_oid.end()) {
      continue;
    }
    ObjectId oid = found->second;
    if (oid < live.size() && live[oid]) {
      result.answers.insert(oid);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  result.query_ns = ns_count(end - start);
  return result;
}

CompareSummary finalize_compare_summary(
    const Options& options,
    const std::string& index_name,
    const std::string& checkpoint,
    std::size_t checkpoint_id,
    std::size_t loaded_count,
    std::size_t initial_count,
    std::size_t insert_count,
    std::size_t delete_count,
    std::size_t live_count,
    long long build_ns,
    long long insert_ns,
    long long delete_ns,
    const SimpleQueryAggregate& aggregate,
    long long oracle_build_ns,
    long long oracle_query_ns,
    std::size_t missing_count,
    std::size_t extra_count) {
  CompareSummary summary;
  summary.workload_mode = options.workload_mode;
  summary.mixed_profile = options.mixed_profile;
  summary.mixed_query_ratio = options.mixed_query_ratio;
  summary.mixed_insert_ratio = options.mixed_insert_ratio;
  summary.mixed_delete_ratio = options.mixed_delete_ratio;
  summary.index = index_name;
  summary.checkpoint = checkpoint;
  summary.checkpoint_id = checkpoint_id;
  summary.loaded_count = loaded_count;
  summary.initial_count = initial_count;
  summary.insert_count = insert_count;
  summary.delete_count = delete_count;
  summary.live_count = live_count;
  summary.query_count = aggregate.latencies_ns.size();
  summary.build_ns = build_ns;
  summary.insert_ns = insert_ns;
  summary.delete_ns = delete_ns;
  summary.insert_tps =
      insert_count == 0 || insert_ns == 0
          ? 0.0
          : static_cast<double>(insert_count) /
                (static_cast<double>(insert_ns) / 1e9);
  summary.delete_tps =
      delete_count == 0 || delete_ns == 0
          ? 0.0
          : static_cast<double>(delete_count) /
                (static_cast<double>(delete_ns) / 1e9);
  long long total_query_ns = 0;
  for (long long value : aggregate.latencies_ns) {
    total_query_ns += value;
  }
  summary.avg_query_ns =
      aggregate.latencies_ns.empty()
          ? 0
          : total_query_ns / static_cast<long long>(aggregate.latencies_ns.size());
  summary.p50_query_ns = percentile_value(aggregate.latencies_ns, 0.50);
  summary.p95_query_ns = percentile_value(aggregate.latencies_ns, 0.95);
  summary.p99_query_ns = percentile_value(aggregate.latencies_ns, 0.99);
  summary.query_tps =
      aggregate.latencies_ns.empty() || total_query_ns == 0
          ? 0.0
          : static_cast<double>(aggregate.latencies_ns.size()) /
                (static_cast<double>(total_query_ns) / 1e9);
  summary.candidates = aggregate.candidates;
  summary.exact_calls = aggregate.exact_calls;
  summary.answers = aggregate.answers;
  summary.candidate_answer_ratio =
      static_cast<double>(aggregate.exact_calls) /
      static_cast<double>(std::max<std::size_t>(aggregate.answers, 1));
  summary.zero_answer_queries = aggregate.zero_answer_queries;
  summary.answers_match_boost = (missing_count == 0 && extra_count == 0) ? 1 : 0;
  summary.missing_count = missing_count;
  summary.extra_count = extra_count;
  summary.oracle_build_ns = oracle_build_ns;
  summary.oracle_query_ns = oracle_query_ns;
  summary.block_size = options.block_size;
  summary.stale_threshold_fraction = options.stale_threshold_fraction;
  summary.delete_compact_fraction = options.delete_compact_fraction;
  return summary;
}

std::vector<std::unordered_set<ObjectId>> compute_oracle_answers(
    const std::vector<GeometryPtr>& geometries,
    const std::vector<ObjectId>& live_ids,
    const std::vector<char>& live,
    const std::vector<QueryCase>& queries,
    long long* build_ns,
    long long* query_ns) {
  RTree oracle = build_rtree_for_ids(geometries, live_ids, live, build_ns);
  std::vector<std::unordered_set<ObjectId>> answers;
  answers.reserve(queries.size());
  long long total_query_ns = 0;
  for (const QueryCase& query_case : queries) {
    SimpleQueryResult result =
        query_boost_index(oracle, geometries, live, query_case);
    total_query_ns += result.query_ns;
    answers.push_back(std::move(result.answers));
  }
  if (query_ns != nullptr) {
    *query_ns = total_query_ns;
  }
  return answers;
}

bool should_check_correctness(const Options& options, std::size_t checkpoint_id);

template <typename QueryFn>
CompareSummary run_generic_checkpoint(
    const Options& options,
    const std::string& index_name,
    const std::string& checkpoint,
    std::size_t checkpoint_id,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<QueryCase>& queries,
    const std::vector<ObjectId>& live_ids,
    const std::vector<char>& live,
    std::size_t initial_count,
    std::size_t insert_count,
    std::size_t delete_count,
    long long build_ns,
    long long insert_ns,
    long long delete_ns,
    QueryFn query_fn) {
  long long oracle_build_ns = 0;
  long long oracle_query_ns = 0;
  const bool do_correctness = should_check_correctness(options, checkpoint_id);
  std::vector<std::unordered_set<ObjectId>> oracle_answers;
  if (do_correctness) {
    oracle_answers = compute_oracle_answers(geometries, live_ids, live, queries,
                                            &oracle_build_ns, &oracle_query_ns);
  }

  SimpleQueryAggregate aggregate;
  std::size_t missing_total = 0;
  std::size_t extra_total = 0;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    SimpleQueryResult result = query_fn(queries[i]);
    add_simple_query_result(aggregate, result);
    if (do_correctness) {
      missing_total += count_missing(oracle_answers[i], result.answers, nullptr);
      extra_total += count_missing(result.answers, oracle_answers[i], nullptr);
    }
  }

  CompareSummary summary = finalize_compare_summary(
      options, index_name, checkpoint, checkpoint_id, geometries.size(),
      initial_count, insert_count, delete_count, live_ids.size(), build_ns,
      insert_ns, delete_ns, aggregate, oracle_build_ns, oracle_query_ns,
      missing_total, extra_total);
  if (!do_correctness) {
    summary.answers_match_boost = -1;
  }
  return summary;
}

std::size_t estimate_deli_bytes(const DynamicExtentIndex& index) {
  std::size_t bytes = index.records().size() * sizeof(DynamicRecord);
  bytes += index.block_count() * sizeof(DynamicBlock);
  bytes += index.total_records() * sizeof(RecordId);
  bytes += index.live_count() * (sizeof(ObjectId) + sizeof(RecordId) + 24);
  return bytes;
}

std::size_t estimate_boost_bytes(std::size_t live_count) {
  return sizeof(RTree) + live_count * (sizeof(RTreeValue) + 32);
}

std::size_t estimate_quadtree_bytes(Quadtree& quadtree) {
  return quadtree.size() * (sizeof(geos::geom::Envelope) + sizeof(void*) + 16) +
         static_cast<std::size_t>(quadtree.depth()) * 96;
}

std::size_t estimate_glin_piece_bytes(std::size_t live_count,
                                      std::size_t piece_count) {
  return live_count * (sizeof(double) + sizeof(Geometry*) + 32) +
         piece_count * sizeof(std::tuple<double, double, double, double>);
}

CompareSummary convert_deli_summary(const CheckpointSummary& source,
                                    const Options& options,
                                    long long build_ns,
                                    std::size_t index_bytes_estimate) {
  CompareSummary summary;
  summary.workload_mode = options.workload_mode;
  summary.mixed_profile = options.mixed_profile;
  summary.mixed_query_ratio = options.mixed_query_ratio;
  summary.mixed_insert_ratio = options.mixed_insert_ratio;
  summary.mixed_delete_ratio = options.mixed_delete_ratio;
  summary.index = "DELI_DYNAMIC_SINGLE";
  summary.checkpoint = source.checkpoint;
  summary.checkpoint_id = source.checkpoint_id;
  summary.loaded_count = source.loaded_count;
  summary.initial_count = source.initial_count;
  summary.insert_count = source.insert_count;
  summary.delete_count = source.delete_count;
  summary.live_count = source.live_count;
  summary.query_count = source.query_count;
  summary.build_ns = build_ns;
  summary.insert_ns = source.insert_ns;
  summary.delete_ns = source.delete_ns;
  summary.insert_tps = source.insert_tps;
  summary.delete_tps = source.delete_tps;
  summary.avg_query_ns = source.avg_query_ns;
  summary.p50_query_ns = source.p50_query_ns;
  summary.p95_query_ns = source.p95_query_ns;
  summary.p99_query_ns = source.p99_query_ns;
  summary.candidates = source.exact_calls;
  summary.exact_calls = source.exact_calls;
  summary.answers = source.answers;
  summary.candidate_answer_ratio = source.candidate_answer_ratio;
  summary.zero_answer_queries = source.zero_answer_queries;
  summary.answers_match_boost = source.answers_match_boost;
  summary.missing_count = source.missing_count;
  summary.extra_count = source.extra_count;
  summary.oracle_build_ns = source.boost_rebuild_ns;
  summary.oracle_query_ns = source.boost_query_ns;
  summary.block_size = options.block_size;
  summary.stale_threshold_fraction = options.stale_threshold_fraction;
  summary.delete_compact_fraction = options.delete_compact_fraction;
  summary.block_count = source.block_count;
  summary.stale_block_count = source.stale_block_count;
  summary.summary_rebuild_count = source.summary_rebuild_count;
  summary.summary_rebuild_ns = source.summary_rebuild_ns;
  summary.block_split_count = source.block_split_count;
  summary.index_bytes_estimate = index_bytes_estimate;
  summary.validate_ok = source.validate_ok;
  summary.note = "DELI bytes are rough vector/hash-table estimates.";
  return summary;
}

CheckpointSummary run_checkpoint(const Options& options,
                                 const std::vector<GeometryPtr>& geometries,
                                 DynamicExtentIndex& index,
                                 const std::vector<QueryCase>& queries,
                                 const std::string& checkpoint,
                                 std::size_t checkpoint_id,
                                 std::size_t initial_count,
                                 std::size_t inserted_count,
                                 std::size_t deleted_count,
                                 long long insert_ns,
                                 long long delete_ns) {
  CheckpointSummary summary;
  summary.checkpoint = checkpoint;
  summary.checkpoint_id = checkpoint_id;
  summary.loaded_count = geometries.size();
  summary.initial_count = initial_count;
  summary.insert_count = inserted_count;
  summary.delete_count = deleted_count;
  summary.live_count = index.live_count();
  summary.total_records = index.total_records();
  summary.dead_records = index.dead_records();
  summary.dead_entry_ratio =
      summary.total_records == 0
          ? 0.0
          : static_cast<double>(summary.dead_records) /
                static_cast<double>(summary.total_records);
  summary.query_count = queries.size();
  summary.block_size = options.block_size;
  summary.block_count = index.block_count();
  summary.stale_threshold_fraction = options.stale_threshold_fraction;
  summary.stale_block_count = index.stale_block_count();
  summary.summary_rebuild_count = index.summary_rebuild_count();
  summary.summary_rebuild_ns = index.summary_rebuild_ns();
  summary.block_split_count = index.block_split_count();
  summary.insert_ns = insert_ns;
  summary.delete_ns = delete_ns;
  summary.insert_tps =
      inserted_count == 0 || insert_ns == 0
          ? 0.0
          : static_cast<double>(inserted_count) /
                (static_cast<double>(insert_ns) / 1e9);
  summary.delete_tps =
      deleted_count == 0 || delete_ns == 0
          ? 0.0
          : static_cast<double>(deleted_count) /
                (static_cast<double>(delete_ns) / 1e9);

  std::string validation_message;
  summary.validate_ok = index.validate_index(&validation_message) ? 1 : 0;
  if (!summary.validate_ok) {
    std::cerr << "validate_index failed at " << checkpoint << ": "
              << validation_message << "\n";
  }

  long long boost_rebuild_ns = 0;
  std::unique_ptr<RTree> rtree;
  const bool do_correctness = should_check_correctness(options, checkpoint_id);
  if (do_correctness) {
    rtree = std::make_unique<RTree>(build_live_rtree(index, &boost_rebuild_ns));
    summary.boost_rebuild_ns = boost_rebuild_ns;
  } else {
    summary.answers_match_boost = -1;
  }

  QueryAggregate aggregate;
  long long boost_query_total_ns = 0;
  std::size_t missing_total = 0;
  std::size_t extra_total = 0;
  ObjectId first_missing = 0;
  ObjectId first_extra = 0;

  for (const QueryCase& query_case : queries) {
    QueryResult result = index.query(query_case);
    add_query_result(aggregate, result);

    if (do_correctness) {
      long long boost_query_ns = 0;
      std::unordered_set<ObjectId> boost_answers =
          query_boost_exact(*rtree, geometries, query_case, &boost_query_ns);
      boost_query_total_ns += boost_query_ns;

      missing_total += count_missing(
          boost_answers, result.answers,
          missing_total == 0 ? &first_missing : nullptr);
      extra_total += count_missing(result.answers, boost_answers,
                                   extra_total == 0 ? &first_extra : nullptr);
    }
  }

  long long total_query_ns = 0;
  for (long long value : aggregate.latencies_ns) {
    total_query_ns += value;
  }
  summary.avg_query_ns =
      aggregate.latencies_ns.empty()
          ? 0
          : total_query_ns / static_cast<long long>(aggregate.latencies_ns.size());
  summary.p50_query_ns = percentile_value(aggregate.latencies_ns, 0.50);
  summary.p95_query_ns = percentile_value(aggregate.latencies_ns, 0.95);
  summary.p99_query_ns = percentile_value(aggregate.latencies_ns, 0.99);
  summary.records_scanned = aggregate.records_scanned;
  summary.visited_blocks = aggregate.visited_blocks;
  summary.skipped_zmax_blocks = aggregate.skipped_zmax_blocks;
  summary.skipped_mbr_blocks = aggregate.skipped_mbr_blocks;
  summary.skipped_block_ratio =
      aggregate.prefix_blocks == 0
          ? 0.0
          : static_cast<double>(aggregate.skipped_zmax_blocks +
                                aggregate.skipped_mbr_blocks) /
                static_cast<double>(aggregate.prefix_blocks);
  summary.interval_candidates = aggregate.interval_candidates;
  summary.mbr_candidates = aggregate.mbr_candidates;
  summary.exact_calls = aggregate.exact_calls;
  summary.answers = aggregate.answers;
  summary.candidate_answer_ratio =
      static_cast<double>(aggregate.exact_calls) /
      static_cast<double>(std::max<std::size_t>(aggregate.answers, 1));
  summary.zero_answer_queries = aggregate.zero_answer_queries;
  if (do_correctness) {
    summary.answers_match_boost = (missing_total == 0 && extra_total == 0) ? 1 : 0;
  }
  summary.missing_count = missing_total;
  summary.extra_count = extra_total;
  summary.boost_query_ns = boost_query_total_ns;

  if (do_correctness && !summary.answers_match_boost) {
    std::cerr << "Answer mismatch at " << checkpoint
              << ": missing=" << missing_total
              << " extra=" << extra_total;
    if (missing_total > 0) {
      std::cerr << " first_missing=" << first_missing;
    }
    if (extra_total > 0) {
      std::cerr << " first_extra=" << first_extra;
    }
    std::cerr << "\n";
    if (options.stop_on_mismatch) {
      throw std::runtime_error("DELI-Dynamic answers do not match Boost");
    }
  }
  return summary;
}

void write_csv(const std::string& path, const Options& options,
               const LoadStats& stats,
               const std::vector<CheckpointSummary>& summaries) {
  if (path.empty()) {
    return;
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }

  output
      << "dataset,index,checkpoint,checkpoint_id,loaded_count,initial_count,"
         "insert_count,delete_count,live_count,total_records,dead_records,"
         "dead_entry_ratio,query_count,block_size,block_count,stale_threshold_fraction,"
         "stale_block_count,summary_rebuild_count,summary_rebuild_ns,"
         "block_split_count,avg_query_ns,p50_query_ns,p95_query_ns,"
         "p99_query_ns,records_scanned,visited_blocks,skipped_zmax_blocks,"
         "skipped_mbr_blocks,skipped_block_ratio,interval_candidates,"
         "mbr_candidates,exact_calls,answers,candidate_answer_ratio,"
         "zero_answer_queries,answers_match_boost,missing_count,extra_count,"
         "boost_rebuild_ns,boost_query_ns,insert_ns,delete_ns,insert_tps,"
         "delete_tps,validate_ok,seed,lines_seen,parse_errors\n";

  output << std::setprecision(17);
  for (const auto& summary : summaries) {
    output << options.dataset_name << ",DELI_DYNAMIC_SINGLE,"
           << summary.checkpoint << "," << summary.checkpoint_id << ","
           << summary.loaded_count << "," << summary.initial_count << ","
           << summary.insert_count << "," << summary.delete_count << ","
           << summary.live_count << "," << summary.total_records << ","
           << summary.dead_records << "," << summary.dead_entry_ratio << ","
           << summary.query_count << "," << summary.block_size << ","
           << summary.block_count << ","
           << summary.stale_threshold_fraction << ","
           << summary.stale_block_count << ","
           << summary.summary_rebuild_count << ","
           << summary.summary_rebuild_ns << "," << summary.block_split_count
           << "," << summary.avg_query_ns << "," << summary.p50_query_ns
           << "," << summary.p95_query_ns << "," << summary.p99_query_ns
           << "," << summary.records_scanned << ","
           << summary.visited_blocks << ","
           << summary.skipped_zmax_blocks << ","
           << summary.skipped_mbr_blocks << ","
           << summary.skipped_block_ratio << ","
           << summary.interval_candidates << "," << summary.mbr_candidates
           << "," << summary.exact_calls << "," << summary.answers << ","
           << summary.candidate_answer_ratio << ","
           << summary.zero_answer_queries << ","
           << summary.answers_match_boost << "," << summary.missing_count
           << "," << summary.extra_count << "," << summary.boost_rebuild_ns
           << "," << summary.boost_query_ns << "," << summary.insert_ns
           << "," << summary.delete_ns << "," << summary.insert_tps << ","
           << summary.delete_tps << "," << summary.validate_ok << ","
           << options.seed << "," << stats.lines_seen << ","
           << stats.parse_errors << "\n";
  }
}

void print_checkpoint_summary(const CheckpointSummary& summary) {
  std::cout << std::fixed << std::setprecision(3)
            << "checkpoint=" << summary.checkpoint
            << " index=DELI_DYNAMIC_SINGLE"
            << " live=" << summary.live_count
            << " total_records=" << summary.total_records
            << " dead_ratio=" << summary.dead_entry_ratio
            << " blocks=" << summary.block_count
            << " stale_blocks=" << summary.stale_block_count
            << " rebuilds=" << summary.summary_rebuild_count
            << " splits=" << summary.block_split_count
            << " avg_query_ns=" << summary.avg_query_ns
            << " p95_query_ns=" << summary.p95_query_ns
            << " records_scanned=" << summary.records_scanned
            << " skipped_block_ratio=" << summary.skipped_block_ratio
            << " candidate_answer_ratio=" << summary.candidate_answer_ratio
            << " answers_match_boost=" << summary.answers_match_boost
            << " missing=" << summary.missing_count
            << " extra=" << summary.extra_count
            << " validate_ok=" << summary.validate_ok << "\n";
}

std::size_t fraction_count(double fraction, std::size_t total) {
  return static_cast<std::size_t>(
      std::floor(fraction * static_cast<double>(total)));
}

std::vector<std::string> split_list(const std::string& value) {
  std::vector<std::string> items;
  std::string current;
  for (char ch : value) {
    if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        items.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    items.push_back(current);
  }
  return items;
}

bool index_enabled(const Options& options, const std::string& index_name) {
  if (options.indexes.empty() || options.indexes == "all" ||
      options.indexes == "ALL") {
    return true;
  }
  for (const std::string& item : split_list(options.indexes)) {
    if (item == index_name) {
      return true;
    }
  }
  return false;
}

bool should_check_correctness(const Options& options,
                              std::size_t checkpoint_id) {
  return options.check_correctness && options.correctness_every_n > 0 &&
         ((checkpoint_id + 1) % options.correctness_every_n == 0);
}

std::vector<std::string> split_colon_fields(const std::string& value) {
  std::vector<std::string> fields;
  std::string current;
  for (char ch : value) {
    if (ch == ':') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  fields.push_back(current);
  return fields;
}

void normalize_mixed_ratios(Options& options) {
  const double ratio_sum = options.mixed_query_ratio +
                           options.mixed_insert_ratio +
                           options.mixed_delete_ratio;
  if (ratio_sum <= 0.0) {
    throw std::runtime_error("mixed workload ratios must sum to > 0");
  }
  options.mixed_query_ratio /= ratio_sum;
  options.mixed_insert_ratio /= ratio_sum;
  options.mixed_delete_ratio /= ratio_sum;
}

std::vector<Options> expand_mixed_profile_options(const Options& base) {
  if (base.mixed_profile_specs.empty()) {
    return {base};
  }

  std::vector<Options> runs;
  for (const std::string& spec : split_list(base.mixed_profile_specs)) {
    std::vector<std::string> fields = split_colon_fields(spec);
    if (fields.size() != 4 || fields[0].empty()) {
      throw std::runtime_error(
          "--mixed_profile_specs entries must be name:query:insert:delete");
    }
    Options item = base;
    item.mixed_profile = fields[0];
    item.mixed_query_ratio = std::stod(fields[1]);
    item.mixed_insert_ratio = std::stod(fields[2]);
    item.mixed_delete_ratio = std::stod(fields[3]);
    normalize_mixed_ratios(item);
    runs.push_back(item);
  }
  if (runs.empty()) {
    return {base};
  }
  return runs;
}

std::vector<Geometry*> raw_ptrs_for_ids(
    const std::vector<GeometryPtr>& geometries,
    const std::vector<ObjectId>& ids) {
  std::vector<Geometry*> raw;
  raw.reserve(ids.size());
  for (ObjectId oid : ids) {
    raw.push_back(geometries[oid].get());
  }
  return raw;
}

void print_compare_summary(const CompareSummary& summary) {
  std::cout << std::fixed << std::setprecision(3)
            << "checkpoint=" << summary.checkpoint
            << " index=" << summary.index
            << " live=" << summary.live_count
            << " avg_query_ms=" << summary.avg_query_ns / 1e6
	            << " p95_query_ms=" << summary.p95_query_ns / 1e6
	            << " insert_tps=" << summary.insert_tps
	            << " delete_tps=" << summary.delete_tps
            << " query_tps=" << summary.query_tps
            << " overall_ops_tps=" << summary.overall_ops_tps
	            << " candidates=" << summary.candidates
            << " candidate_answer_ratio=" << summary.candidate_answer_ratio
            << " answers_match_boost=" << summary.answers_match_boost
            << " missing=" << summary.missing_count
            << " extra=" << summary.extra_count
            << " index_bytes_estimate=" << summary.index_bytes_estimate
            << "\n";
}

struct MaintenanceSnapshot {
  std::size_t summary_rebuild_count = 0;
  long long summary_rebuild_ns = 0;
  std::size_t local_compaction_count = 0;
  long long local_compaction_ns = 0;
};

void annotate_stage_maintenance(std::vector<CompareSummary>& rows) {
  std::unordered_map<std::string, MaintenanceSnapshot> previous_by_index;
  for (CompareSummary& row : rows) {
    row.summary_rebuild_count_total = row.summary_rebuild_count;
    row.summary_rebuild_ns_total = row.summary_rebuild_ns;
    row.local_compaction_count_total = row.local_compaction_count;
    row.local_compaction_ns_total = row.local_compaction_ns;

    auto found = previous_by_index.find(row.index);
    if (found == previous_by_index.end()) {
      row.summary_rebuild_count_stage = row.summary_rebuild_count_total;
      row.summary_rebuild_ns_stage = row.summary_rebuild_ns_total;
      row.local_compaction_count_stage = row.local_compaction_count_total;
      row.local_compaction_ns_stage = row.local_compaction_ns_total;
    } else {
      const MaintenanceSnapshot& previous = found->second;
      row.summary_rebuild_count_stage =
          row.summary_rebuild_count_total >= previous.summary_rebuild_count
              ? row.summary_rebuild_count_total - previous.summary_rebuild_count
              : row.summary_rebuild_count_total;
      row.summary_rebuild_ns_stage =
          row.summary_rebuild_ns_total >= previous.summary_rebuild_ns
              ? row.summary_rebuild_ns_total - previous.summary_rebuild_ns
              : row.summary_rebuild_ns_total;
      row.local_compaction_count_stage =
          row.local_compaction_count_total >= previous.local_compaction_count
              ? row.local_compaction_count_total -
                    previous.local_compaction_count
              : row.local_compaction_count_total;
      row.local_compaction_ns_stage =
          row.local_compaction_ns_total >= previous.local_compaction_ns
              ? row.local_compaction_ns_total - previous.local_compaction_ns
              : row.local_compaction_ns_total;
    }

    if (row.local_compaction_count_stage > 0) {
      row.avg_local_compaction_us_stage =
          static_cast<double>(row.local_compaction_ns_stage) /
          static_cast<double>(row.local_compaction_count_stage) / 1000.0;
    }

    previous_by_index[row.index] = MaintenanceSnapshot{
        row.summary_rebuild_count_total, row.summary_rebuild_ns_total,
        row.local_compaction_count_total, row.local_compaction_ns_total};
  }
}

void write_compare_csv(const std::string& path,
                       const Options& options,
                       const LoadStats& stats,
                       const std::vector<CompareSummary>& rows) {
  if (path.empty()) {
    return;
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }
  output
      << "dataset,workload_mode,mixed_profile,operation_count,checkpoint_ops,"
         "mixed_query_ratio,mixed_insert_ratio,mixed_delete_ratio,"
	         "index,checkpoint,checkpoint_id,loaded_count,initial_count,"
	         "insert_count,delete_count,live_count,query_count,build_ns,insert_ns,"
         "delete_ns,p95_insert_ns,p99_insert_ns,p95_delete_ns,p99_delete_ns,"
         "insert_tps,delete_tps,query_tps,overall_ops_tps,avg_query_ns,p50_query_ns,"
	         "p95_query_ns,p99_query_ns,candidates,exact_calls,answers,"
         "candidate_answer_ratio,zero_answer_queries,answers_match_boost,"
         "missing_count,extra_count,oracle_build_ns,oracle_query_ns,"
         "block_size,stale_threshold_fraction,delete_compact_fraction,"
         "block_count,stale_block_count,"
         "summary_rebuild_count,summary_rebuild_ns,"
         "summary_rebuild_count_total,summary_rebuild_ns_total,"
         "summary_rebuild_count_stage,summary_rebuild_ns_stage,"
         "block_split_count,local_compaction_count,local_compaction_ns,"
         "local_compaction_count_total,local_compaction_ns_total,"
         "local_compaction_count_stage,local_compaction_ns_stage,"
         "avg_local_compaction_us_stage,local_delta_bound,"
         "delete_compact_bound,max_local_delta_size,avg_local_delta_size,"
         "blocks_with_delta,tombstone_ratio,pieces,"
         "tree_size,tree_depth,tree_nodes,index_bytes_estimate,success_count,"
         "failed_count,validate_ok,seed,lines_seen,parse_errors,note\n";
  output << std::setprecision(17);
  for (const CompareSummary& row : rows) {
    output << options.dataset_name << "," << row.workload_mode << ","
           << row.mixed_profile << "," << row.operation_count << ","
           << row.checkpoint_ops << "," << row.mixed_query_ratio << ","
           << row.mixed_insert_ratio << "," << row.mixed_delete_ratio << ","
           << row.index << "," << row.checkpoint << "," << row.checkpoint_id << ","
	           << row.loaded_count << "," << row.initial_count << ","
	           << row.insert_count << "," << row.delete_count << ","
	           << row.live_count << "," << row.query_count << ","
	           << row.build_ns << "," << row.insert_ns << "," << row.delete_ns
           << "," << row.p95_insert_ns << "," << row.p99_insert_ns
           << "," << row.p95_delete_ns << "," << row.p99_delete_ns
           << "," << row.insert_tps << "," << row.delete_tps << ","
           << row.query_tps << "," << row.overall_ops_tps << ","
	           << row.avg_query_ns << "," << row.p50_query_ns << ","
           << row.p95_query_ns << "," << row.p99_query_ns << ","
           << row.candidates << "," << row.exact_calls << ","
           << row.answers << "," << row.candidate_answer_ratio << ","
           << row.zero_answer_queries << "," << row.answers_match_boost
           << "," << row.missing_count << "," << row.extra_count << ","
           << row.oracle_build_ns << "," << row.oracle_query_ns << ","
           << row.block_size << "," << row.stale_threshold_fraction << ","
           << row.delete_compact_fraction << "," << row.block_count << ","
           << row.stale_block_count << ","
           << row.summary_rebuild_count << "," << row.summary_rebuild_ns
           << "," << row.summary_rebuild_count_total << ","
           << row.summary_rebuild_ns_total << ","
           << row.summary_rebuild_count_stage << ","
           << row.summary_rebuild_ns_stage << "," << row.block_split_count
           << "," << row.local_compaction_count << ","
           << row.local_compaction_ns << ","
           << row.local_compaction_count_total << ","
           << row.local_compaction_ns_total << ","
           << row.local_compaction_count_stage << ","
           << row.local_compaction_ns_stage << ","
           << row.avg_local_compaction_us_stage << ","
           << row.local_delta_bound << "," << row.delete_compact_bound << ","
           << row.max_local_delta_size << "," << row.avg_local_delta_size
           << "," << row.blocks_with_delta << "," << row.tombstone_ratio
           << "," << row.pieces << ","
           << row.tree_size << "," << row.tree_depth << ","
           << row.tree_nodes << "," << row.index_bytes_estimate << ","
           << row.success_count << "," << row.failed_count << ","
           << row.validate_ok << "," << options.seed << ","
           << stats.lines_seen << "," << stats.parse_errors << ",\""
           << row.note << "\"\n";
  }
}

struct MixedOperation {
  char kind = 'q';  // q=query, i=insert, d=delete
  ObjectId oid = 0;
  std::size_t query_index = 0;
};

struct LiveReplayState {
  std::vector<char> live;
  std::vector<ObjectId> live_ids;
  std::vector<std::size_t> position;

  LiveReplayState(std::size_t object_count,
                  const std::vector<ObjectId>& initial_ids)
      : live(object_count, 0),
        live_ids(initial_ids),
        position(object_count, std::numeric_limits<std::size_t>::max()) {
    for (std::size_t i = 0; i < live_ids.size(); ++i) {
      ObjectId oid = live_ids[i];
      live[oid] = 1;
      position[oid] = i;
    }
  }

  void insert(ObjectId oid) {
    if (oid >= live.size() || live[oid]) {
      return;
    }
    live[oid] = 1;
    position[oid] = live_ids.size();
    live_ids.push_back(oid);
  }

  void erase(ObjectId oid) {
    if (oid >= live.size() || !live[oid]) {
      return;
    }
    const std::size_t pos = position[oid];
    const ObjectId last = live_ids.back();
    live_ids[pos] = last;
    position[last] = pos;
    live_ids.pop_back();
    live[oid] = 0;
    position[oid] = std::numeric_limits<std::size_t>::max();
  }
};
//生成mixed workload 混合查询负载，这个会生成一个固定的操作序列，如：

/*
1: query query[0]
2: query query[1]
3: insert object 812345
4: query query[2]
5: delete object 12345
6: query query[3]
...
100000: delete object 456789
*/

/*所有方法会复用这个操作序列，所以能保证
所以不同方法的：
第几步是 query
第几步是 insert
插入哪个 object_id
第几步是 delete
删除哪个 object_id
都是一样的。
为什么删除对象也能一样？因为生成操作序列时，代码内部维护了一个“模拟 live set”。
简单说：
初始 live set = bulk-load 的 50% 对象
insert 时：从未插入对象池里取一个 object_id，并加入 live set
delete 时：从当前 live set 里选一个 object_id，并从 live set 删除
这个过程只发生在生成 workload 时。生成完后，delete 的 object_id 已经固定了。后面每个方法只是照着执行：
delete object 12345
而不是自己再随机选要删谁。
所以公平性来自两层：
1. seed 固定 -> 生成出来的 operations 固定
2. operations 先生成好 -> 所有方法复用同一条操作序列
*/

std::vector<MixedOperation> generate_mixed_operations(
    const Options& options,
    std::size_t object_count,
    std::size_t initial_count,
    std::size_t query_count) {
  std::vector<ObjectId> insert_pool;
  insert_pool.reserve(object_count > initial_count ? object_count - initial_count
                                                   : 0);
  for (ObjectId oid = static_cast<ObjectId>(initial_count);
       oid < static_cast<ObjectId>(object_count); ++oid) {
    insert_pool.push_back(oid);
  }

  std::mt19937_64 rng(options.seed ^ 0x9e3779b97f4a7c15ULL);
  std::shuffle(insert_pool.begin(), insert_pool.end(), rng);
  std::uniform_real_distribution<double> probability(0.0, 1.0);

  std::vector<ObjectId> initial_ids(initial_count);
  std::iota(initial_ids.begin(), initial_ids.end(), ObjectId{0});
  LiveReplayState state(object_count, initial_ids);

  std::vector<MixedOperation> operations;
  operations.reserve(options.mixed_operations);
  std::size_t insert_cursor = 0;

  for (std::size_t op_id = 0; op_id < options.mixed_operations; ++op_id) {
    const double r = probability(rng);
    MixedOperation op;
    if (r < options.mixed_query_ratio || query_count == 0) {
      op.kind = 'q';
      op.query_index = query_count == 0 ? 0 : op_id % query_count;
    } else if (r < options.mixed_query_ratio + options.mixed_insert_ratio &&
               insert_cursor < insert_pool.size()) {
      op.kind = 'i';
      op.oid = insert_pool[insert_cursor++];
      state.insert(op.oid);
    } else if (!state.live_ids.empty()) {
      op.kind = 'd';
      std::uniform_int_distribution<std::size_t> pick(
          0, state.live_ids.size() - 1);
      op.oid = state.live_ids[pick(rng)];
      state.erase(op.oid);
    } else {
      op.kind = 'q';
      op.query_index = query_count == 0 ? 0 : op_id % query_count;
    }
    operations.push_back(op);
  }
  return operations;
}

SimpleQueryResult simple_from_deli_query(const QueryResult& source) {
  SimpleQueryResult result;
  result.query_ns = source.query_ns;
  result.candidates = source.exact_calls;
  result.exact_calls = source.exact_calls;
  result.answers = source.answers;
  return result;
}

template <typename QueryFn>
void mixed_correctness_check(const Options& options,
                             const std::vector<GeometryPtr>& geometries,
                             const std::vector<QueryCase>& queries,
                             const LiveReplayState& state,
                             std::size_t checkpoint_id,
                             QueryFn query_fn,
                             long long* oracle_build_ns,
                             long long* oracle_query_ns,
                             std::size_t* missing_total,
                             std::size_t* extra_total) {
  *oracle_build_ns = 0;
  *oracle_query_ns = 0;
  *missing_total = 0;
  *extra_total = 0;
  if (!should_check_correctness(options, checkpoint_id)) {
    return;
  }

  std::vector<std::unordered_set<ObjectId>> oracle_answers =
      compute_oracle_answers(geometries, state.live_ids, state.live, queries,
                             oracle_build_ns, oracle_query_ns);
  for (std::size_t i = 0; i < queries.size(); ++i) {
    SimpleQueryResult result = query_fn(queries[i], state.live);
    *missing_total += count_missing(oracle_answers[i], result.answers, nullptr);
    *extra_total += count_missing(result.answers, oracle_answers[i], nullptr);
  }
}

template <typename QueryFn, typename InsertFn, typename DeleteFn,
          typename AnnotateFn>
void run_mixed_workload_for_index(
    const Options& options,
    const std::string& index_name,
    const std::vector<GeometryPtr>& geometries,
    const std::vector<QueryCase>& queries,
    const std::vector<ObjectId>& initial_ids,
    const std::vector<MixedOperation>& operations,
    long long build_ns,
    QueryFn query_fn,
    InsertFn insert_fn,
    DeleteFn delete_fn,
    AnnotateFn annotate_fn,
    std::vector<CompareSummary>& rows) {
  LiveReplayState state(geometries.size(), initial_ids);
  SimpleQueryAggregate aggregate;
  long long interval_insert_ns = 0;
  long long interval_delete_ns = 0;
  std::vector<long long> interval_insert_latencies_ns;
  std::vector<long long> interval_delete_latencies_ns;
  std::size_t interval_insert_count = 0;
  std::size_t interval_delete_count = 0;
  std::size_t interval_success = 0;
  std::size_t interval_failed = 0;
  std::size_t interval_begin = 0;
  std::size_t checkpoint_id = 0;

  auto emit_checkpoint = [&](std::size_t completed_ops) {
    long long oracle_build_ns = 0;
    long long oracle_query_ns = 0;
    std::size_t missing_total = 0;
    std::size_t extra_total = 0;
    const bool do_correctness = should_check_correctness(options, checkpoint_id);
    mixed_correctness_check(options, geometries, queries, state, checkpoint_id,
                            query_fn,
                            &oracle_build_ns, &oracle_query_ns,
                            &missing_total, &extra_total);

    CompareSummary row = finalize_compare_summary(
        options, index_name,
        "mixed_" + std::to_string(completed_ops), checkpoint_id,
        geometries.size(), initial_ids.size(), interval_insert_count,
        interval_delete_count, state.live_ids.size(), build_ns,
        interval_insert_ns, interval_delete_ns, aggregate, oracle_build_ns,
        oracle_query_ns, missing_total, extra_total);
    row.workload_mode = "mixed";
    row.mixed_profile = options.mixed_profile;
	    row.operation_count = completed_ops;
	    row.checkpoint_ops = completed_ops - interval_begin;
	    row.success_count = interval_success;
	    row.failed_count = interval_failed;
    row.p95_insert_ns = percentile_value(interval_insert_latencies_ns, 0.95);
    row.p99_insert_ns = percentile_value(interval_insert_latencies_ns, 0.99);
    row.p95_delete_ns = percentile_value(interval_delete_latencies_ns, 0.95);
    row.p99_delete_ns = percentile_value(interval_delete_latencies_ns, 0.99);
    const long long foreground_ns =
        row.query_count == 0 ? 0 : row.avg_query_ns *
                                   static_cast<long long>(row.query_count);
    const long long total_foreground_ns =
        foreground_ns + interval_insert_ns + interval_delete_ns;
    row.overall_ops_tps =
        row.checkpoint_ops == 0 || total_foreground_ns == 0
            ? 0.0
            : static_cast<double>(row.checkpoint_ops) /
                  (static_cast<double>(total_foreground_ns) / 1e9);
	    if (!do_correctness) {
	      row.answers_match_boost = -1;
	    }
    annotate_fn(row);
    rows.push_back(row);
    print_compare_summary(rows.back());

    ++checkpoint_id;
    interval_begin = completed_ops;
	    aggregate = SimpleQueryAggregate();
	    interval_insert_ns = 0;
	    interval_delete_ns = 0;
    interval_insert_latencies_ns.clear();
    interval_delete_latencies_ns.clear();
	    interval_insert_count = 0;
    interval_delete_count = 0;
    interval_success = 0;
    interval_failed = 0;
  };

  for (std::size_t i = 0; i < operations.size(); ++i) {
    const MixedOperation& op = operations[i];
    if (op.kind == 'q') {
      add_simple_query_result(aggregate,
                              query_fn(queries[op.query_index], state.live));
    } else if (op.kind == 'i') {
      auto start = std::chrono::high_resolution_clock::now();
	      bool ok = insert_fn(op.oid);
	      auto end = std::chrono::high_resolution_clock::now();
      const long long op_ns = ns_count(end - start);
      interval_insert_ns += op_ns;
      interval_insert_latencies_ns.push_back(op_ns);
      ++interval_insert_count;
      ok ? ++interval_success : ++interval_failed;
      state.insert(op.oid);
    } else if (op.kind == 'd') {
      auto start = std::chrono::high_resolution_clock::now();
	      bool ok = delete_fn(op.oid);
	      auto end = std::chrono::high_resolution_clock::now();
      const long long op_ns = ns_count(end - start);
      interval_delete_ns += op_ns;
      interval_delete_latencies_ns.push_back(op_ns);
      ++interval_delete_count;
      ok ? ++interval_success : ++interval_failed;
      state.erase(op.oid);
    }

    const std::size_t completed_ops = i + 1;
    if (completed_ops % options.mixed_checkpoint_interval == 0 ||
        completed_ops == operations.size()) {
      emit_checkpoint(completed_ops);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    LoadStats stats;
    auto load_start = std::chrono::high_resolution_clock::now();
    std::vector<GeometryPtr> geometries = load_wkt_csv(options, reader, stats);
    auto load_end = std::chrono::high_resolution_clock::now();
    if (geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }

    std::vector<GeometryPtr> owned_queries;
    std::vector<QueryCase> queries =
        load_query_file(options, *factory, owned_queries);

    std::vector<ObjectId> all_ids(geometries.size());
    std::iota(all_ids.begin(), all_ids.end(), ObjectId{0});

    std::size_t initial_count =
        fraction_count(options.initial_fraction, geometries.size());
    initial_count = std::max<std::size_t>(1, initial_count);
    initial_count = std::min(initial_count, geometries.size());

    std::vector<ObjectId> initial_ids(all_ids.begin(),
                                      all_ids.begin() + initial_count);
    std::vector<ObjectId> insert_ids(all_ids.begin() + initial_count,
                                     all_ids.end());
    std::mt19937_64 rng(options.seed);
    std::shuffle(insert_ids.begin(), insert_ids.end(), rng);

    std::size_t requested_insert_count =
        fraction_count(options.insert_fraction, geometries.size());
    std::size_t insert_count =
        std::min(requested_insert_count, insert_ids.size());
    insert_ids.resize(insert_count);

    std::vector<ObjectId> live_after_bulkload = initial_ids;
    std::vector<char> live_bulkload(geometries.size(), 0);
    for (ObjectId oid : live_after_bulkload) {
      live_bulkload[oid] = 1;
    }

    std::vector<ObjectId> live_after_insert = live_after_bulkload;
    live_after_insert.insert(live_after_insert.end(), insert_ids.begin(),
                             insert_ids.end());
    std::vector<char> live_insert(geometries.size(), 0);
    for (ObjectId oid : live_after_insert) {
      live_insert[oid] = 1;
    }

    std::vector<ObjectId> delete_ids = live_after_insert;
    std::shuffle(delete_ids.begin(), delete_ids.end(), rng);
    std::size_t requested_delete_count =
        fraction_count(options.delete_fraction, geometries.size());
    std::size_t delete_count =
        std::min(requested_delete_count, delete_ids.size());
    delete_ids.resize(delete_count);

    std::vector<char> live_delete = live_insert;
    for (ObjectId oid : delete_ids) {
      live_delete[oid] = 0;
    }
    std::vector<ObjectId> live_after_delete;
    live_after_delete.reserve(live_after_insert.size());
    for (ObjectId oid : live_after_insert) {
      if (live_delete[oid]) {
        live_after_delete.push_back(oid);
      }
    }

    std::unordered_map<Geometry*, ObjectId> geometry_to_oid =
        build_geometry_to_oid(geometries);
    std::vector<GeometryMeta> geometry_metadata =
        build_geometry_metadata(geometries, options);

    std::cout << "dataset=" << options.dataset_name
              << " benchmark=dynamic_compare"
              << " loaded=" << geometries.size()
              << " initial=" << initial_count
              << " insert=" << insert_count
              << " delete=" << delete_count
              << " queries=" << queries.size()
              << " deli_block_size=" << options.block_size
              << " deli_stale=" << options.stale_threshold_fraction
              << " local_delta_bound="
              << (options.local_delta_bound == 0
                      ? std::max<std::size_t>(64, options.block_size / 4)
                      : options.local_delta_bound)
              << " delete_compact_fraction="
              << options.delete_compact_fraction
              << " delete_compact_bound="
              << (options.delete_compact_fraction <= 0.0
                      ? 0
                      : std::max<std::size_t>(
                            1, static_cast<std::size_t>(
                                   std::ceil(options.delete_compact_fraction *
                                             static_cast<double>(
                                                 options.block_size)))))
              << " load_ms=" << ns_count(load_end - load_start) / 1e6
              << "\n";

    std::vector<CompareSummary> rows;

    if (options.workload_mode == "mixed") {
      std::vector<Options> mixed_option_runs =
          expand_mixed_profile_options(options);
      for (const Options& active_options : mixed_option_runs) {
        const Options& options = active_options;
      std::vector<MixedOperation> operations = generate_mixed_operations(
          options, geometries.size(), initial_count, queries.size());

      auto annotate_deli_dynamic = [&](DynamicExtentIndex& idx,
                                       CompareSummary& row) {
        row.block_count = idx.block_count();
        row.stale_block_count = idx.stale_block_count();
        row.summary_rebuild_count = idx.summary_rebuild_count();
        row.summary_rebuild_ns = idx.summary_rebuild_ns();
        row.block_split_count = idx.block_split_count();
        row.index_bytes_estimate = estimate_deli_bytes(idx);
        row.tombstone_ratio =
            idx.total_records() == 0
                ? 0.0
                : static_cast<double>(idx.dead_records()) /
                      static_cast<double>(idx.total_records());
        std::string validation_message;
        row.validate_ok = idx.validate_index(&validation_message) ? 1 : 0;
        row.note = "Mixed workload; DELI-Dynamic-Single.";
      };

      if (index_enabled(options, "DELI_DYNAMIC_SINGLE")) {
        DynamicExtentIndex mixed_index(options, geometries);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "DELI_DYNAMIC_SINGLE", geometries, queries, initial_ids,
            operations, mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>&) {
              return simple_from_deli_query(mixed_index.query(query_case));
            },
            [&](ObjectId oid) {
              mixed_index.insert(oid);
              return true;
            },
            [&](ObjectId oid) { return mixed_index.erase(oid); },
            [&](CompareSummary& row) { annotate_deli_dynamic(mixed_index, row); },
            rows);
      }

      if (index_enabled(options, "DELI_ALEX")) {
        DeliAlexIndex mixed_index(options, geometries, geometry_metadata);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "DELI_ALEX", geometries, queries, initial_ids, operations,
            mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>&) {
              return mixed_index.query(query_case);
            },
            [&](ObjectId oid) { return mixed_index.insert(oid); },
            [&](ObjectId oid) { return mixed_index.erase(oid); },
            [&](CompareSummary& row) {
              row.block_count = mixed_index.group_count();
              row.tree_nodes = mixed_index.leaf_count();
              row.stale_block_count = mixed_index.stale_leaf_count();
              row.summary_rebuild_count =
                  mixed_index.summary_rebuild_count() +
                  mixed_index.group_directory_rebuild_count();
              row.summary_rebuild_ns =
                  mixed_index.summary_rebuild_ns() +
                  mixed_index.group_directory_rebuild_ns();
              row.index_bytes_estimate = mixed_index.index_bytes_estimate();
              row.note = "Mixed workload; DELI-ALEX.";
            },
            rows);
      }

      auto run_mixed_hybrid = [&](const std::string& name, auto& mixed_index,
                                  long long mixed_build_ns) {
        run_mixed_workload_for_index(
            options, name, geometries, queries, initial_ids, operations,
            mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>&) {
              return mixed_index.query(query_case);
            },
            [&](ObjectId oid) { return mixed_index.insert(oid); },
            [&](ObjectId oid) { return mixed_index.erase(oid); },
            [&](CompareSummary& row) {
              row.block_count = mixed_index.block_count();
              row.tree_nodes = mixed_index.leaf_count();
              row.stale_block_count = mixed_index.stale_block_count();
              row.summary_rebuild_count = mixed_index.summary_rebuild_count();
              row.summary_rebuild_ns = mixed_index.summary_rebuild_ns();
              row.block_split_count = mixed_index.block_split_count();
              row.index_bytes_estimate = mixed_index.index_bytes_estimate();
              std::string validation_message;
              row.validate_ok =
                  mixed_index.validate_index(&validation_message) ? 1 : 0;
              row.note = "Mixed workload; " + name + ".";
            },
            rows);
      };

      if (index_enabled(options, "DELI_ALEX_HYBRID")) {
        DeliAlexHybridIndex mixed_index(options, geometries, geometry_metadata);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        run_mixed_hybrid("DELI_ALEX_HYBRID", mixed_index, ns_count(end - start));
      }
      if (index_enabled(options, "DELI_ALEX_HYBRID_BUF")) {
        DeliAlexHybridBufferedIndex mixed_index(options, geometries,
                                                geometry_metadata, false);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        run_mixed_hybrid("DELI_ALEX_HYBRID_BUF", mixed_index,
                         ns_count(end - start));
      }
      if (index_enabled(options, "DELI_ALEX_HYBRID_BOUNDED")) {
        DeliAlexHybridBufferedIndex mixed_index(options, geometries,
                                                geometry_metadata, true);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        run_mixed_hybrid("DELI_ALEX_HYBRID_BOUNDED", mixed_index,
                         ns_count(end - start));
      }
      if (index_enabled(options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED")) {
        DeliAlexHybridLocalBoundedIndex mixed_index(options, geometries,
                                                    geometry_metadata);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.bulk_load(initial_ids);
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED", geometries, queries,
            initial_ids, operations, mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>&) {
              return mixed_index.query(query_case);
            },
            [&](ObjectId oid) { return mixed_index.insert(oid); },
            [&](ObjectId oid) { return mixed_index.erase(oid); },
            [&](CompareSummary& row) {
              row.block_count = mixed_index.block_count();
              row.tree_nodes = mixed_index.leaf_count();
              row.stale_block_count = mixed_index.stale_block_count();
              row.summary_rebuild_count = mixed_index.summary_rebuild_count();
              row.summary_rebuild_ns = mixed_index.summary_rebuild_ns();
              row.block_split_count = mixed_index.block_split_count();
              row.local_compaction_count =
                  mixed_index.local_compaction_count();
              row.local_compaction_ns = mixed_index.local_compaction_ns();
              row.local_delta_bound = mixed_index.local_delta_bound();
              row.delete_compact_bound = mixed_index.delete_compact_bound();
              row.max_local_delta_size = mixed_index.max_local_delta_size();
              row.avg_local_delta_size = mixed_index.avg_local_delta_size();
              row.blocks_with_delta = mixed_index.blocks_with_delta();
              row.tombstone_ratio = mixed_index.tombstone_ratio();
              row.index_bytes_estimate = mixed_index.index_bytes_estimate();
              std::string validation_message;
              row.validate_ok =
                  mixed_index.validate_index(&validation_message) ? 1 : 0;
              row.note =
                  "Mixed workload; DELI-ALEX-Hybrid-LocalBounded.";
            },
            rows);
      }

      if (index_enabled(options, "Boost_Rtree")) {
        auto start = std::chrono::high_resolution_clock::now();
        RTree mixed_index =
            build_rtree_for_ids(geometries, initial_ids, live_bulkload, nullptr);
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "Boost_Rtree", geometries, queries, initial_ids,
            operations, mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>& live) {
              return query_boost_index(mixed_index, geometries, live,
                                       query_case);
            },
            [&](ObjectId oid) {
              mixed_index.insert(std::make_pair(
                  boost_box_from_envelope(
                      *geometries[oid]->getEnvelopeInternal()),
                  oid));
              return true;
            },
            [&](ObjectId oid) {
              return mixed_index.remove(std::make_pair(
                         boost_box_from_envelope(
                             *geometries[oid]->getEnvelopeInternal()),
                         oid)) > 0;
            },
            [&](CompareSummary& row) {
              row.index_bytes_estimate = estimate_boost_bytes(row.live_count);
              row.note = "Mixed workload; Boost R-tree.";
            },
            rows);
      }

      if (index_enabled(options, "GEOS_Quadtree")) {
        Quadtree mixed_index;
        auto start = std::chrono::high_resolution_clock::now();
        for (ObjectId oid : initial_ids) {
          Geometry* geometry = geometries[oid].get();
          mixed_index.insert(geometry->getEnvelopeInternal(), geometry);
        }
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "GEOS_Quadtree", geometries, queries, initial_ids,
            operations, mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>& live) {
              return query_quadtree_index(mixed_index, geometry_to_oid,
                                          geometries, live, query_case);
            },
            [&](ObjectId oid) {
              Geometry* geometry = geometries[oid].get();
              mixed_index.insert(geometry->getEnvelopeInternal(), geometry);
              return true;
            },
            [&](ObjectId oid) {
              Geometry* geometry = geometries[oid].get();
              return mixed_index.remove(geometry->getEnvelopeInternal(),
                                        geometry);
            },
            [&](CompareSummary& row) {
              row.tree_size = mixed_index.size();
              row.tree_depth = static_cast<std::size_t>(mixed_index.depth());
              row.index_bytes_estimate = estimate_quadtree_bytes(mixed_index);
              row.note = "Mixed workload; GEOS Quadtree.";
            },
            rows);
      }

      if (index_enabled(options, "GLIN_PIECEWISE")) {
        alex::Glin<double, Geometry*> mixed_index;
        std::vector<std::tuple<double, double, double, double>> mixed_pieces;
        std::vector<Geometry*> glin_initial =
            raw_ptrs_for_ids(geometries, initial_ids);
        auto start = std::chrono::high_resolution_clock::now();
        mixed_index.glin_bulk_load(
            glin_initial, options.piece_limit, "z", options.cell_xmin,
            options.cell_ymin, options.cell_size, options.cell_size,
            mixed_pieces);
        auto end = std::chrono::high_resolution_clock::now();
        const long long mixed_build_ns = ns_count(end - start);
        run_mixed_workload_for_index(
            options, "GLIN_PIECEWISE", geometries, queries, initial_ids,
            operations, mixed_build_ns,
            [&](const QueryCase& query_case, const std::vector<char>& live) {
              return query_glin_piece_index(mixed_index, mixed_pieces,
                                            geometry_to_oid, live, options,
                                            query_case);
            },
            [&](ObjectId oid) {
              Geometry* geometry = geometries[oid].get();
              geos::geom::Envelope* envelope =
                  const_cast<geos::geom::Envelope*>(
                      geometry->getEnvelopeInternal());
              auto result = mixed_index.glin_insert(
                  std::make_tuple(geometry, envelope), "z", options.cell_xmin,
                  options.cell_ymin, options.cell_size, options.cell_size,
                  options.piece_limit, mixed_pieces);
              return result.second;
            },
            [&](ObjectId oid) {
              Geometry* geometry = geometries[oid].get();
              return mixed_index.erase(geometry, "z", options.cell_xmin,
                                       options.cell_ymin, options.cell_size,
                                       options.cell_size, options.piece_limit,
                                       mixed_pieces) > 0;
            },
            [&](CompareSummary& row) {
              row.pieces = mixed_pieces.size();
              row.index_bytes_estimate =
                  estimate_glin_piece_bytes(row.live_count,
                                            mixed_pieces.size());
              row.note = "Mixed workload; GLIN-piece.";
            },
            rows);
      }
      }

      annotate_stage_maintenance(rows);
      write_compare_csv(options.output_csv, options, stats, rows);
      if (!options.output_csv.empty()) {
        std::cout << "CSV: " << options.output_csv << "\n";
      }
      return 0;
    }

    // DELI-Dynamic-Single.
    if (index_enabled(options, "DELI_DYNAMIC_SINGLE")) {
    DynamicExtentIndex index(options, geometries);
    auto build_start = std::chrono::high_resolution_clock::now();
    index.bulk_load(initial_ids);
    auto build_end = std::chrono::high_resolution_clock::now();
    long long build_ns = ns_count(build_end - build_start);

    CheckpointSummary deli_bulkload =
        run_checkpoint(options, geometries, index, queries, "after_bulkload",
                       0, initial_count, 0, 0, 0, 0);
    rows.push_back(convert_deli_summary(
        deli_bulkload, options, build_ns, estimate_deli_bytes(index)));
    print_compare_summary(rows.back());

    long long insert_ns = 0;
    if (!insert_ids.empty()) {
      auto insert_start = std::chrono::high_resolution_clock::now();
      std::size_t op_count = 0;
      for (ObjectId oid : insert_ids) {
        index.insert(oid);
        ++op_count;
        if (options.validate_every > 0 &&
            op_count % options.validate_every == 0) {
          std::string message;
          if (!index.validate_index(&message)) {
            throw std::runtime_error("validate_index after insert failed: " +
                                     message);
          }
        }
      }
      auto insert_end = std::chrono::high_resolution_clock::now();
      insert_ns = ns_count(insert_end - insert_start);
    }

    CheckpointSummary deli_insert =
        run_checkpoint(options, geometries, index, queries, "after_insert", 1,
                       initial_count, insert_count, 0, insert_ns, 0);
    rows.push_back(convert_deli_summary(
        deli_insert, options, build_ns, estimate_deli_bytes(index)));
    print_compare_summary(rows.back());

    long long delete_ns = 0;
    if (!delete_ids.empty()) {
      auto delete_start = std::chrono::high_resolution_clock::now();
      std::size_t op_count = 0;
      for (ObjectId oid : delete_ids) {
        index.erase(oid);
        ++op_count;
        if (options.validate_every > 0 &&
            op_count % options.validate_every == 0) {
          std::string message;
          if (!index.validate_index(&message)) {
            throw std::runtime_error("validate_index after delete failed: " +
                                     message);
          }
        }
      }
      auto delete_end = std::chrono::high_resolution_clock::now();
      delete_ns = ns_count(delete_end - delete_start);
    }

    CheckpointSummary deli_delete =
        run_checkpoint(options, geometries, index, queries, "after_delete", 2,
                       initial_count, insert_count, delete_count, insert_ns,
                       delete_ns);
    rows.push_back(convert_deli_summary(
        deli_delete, options, build_ns, estimate_deli_bytes(index)));
    print_compare_summary(rows.back());
    }

    // DELI-ALEX: existing ALEX leaf layout + DELI leaf summaries.
    if (index_enabled(options, "DELI_ALEX")) {
    DeliAlexIndex deli_alex(options, geometries, geometry_metadata);
    auto deli_alex_build_start = std::chrono::high_resolution_clock::now();
    deli_alex.bulk_load(initial_ids);
    auto deli_alex_build_end = std::chrono::high_resolution_clock::now();
    long long deli_alex_build_ns =
        ns_count(deli_alex_build_end - deli_alex_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX", "after_bulkload", 0, geometries, queries,
        live_after_bulkload, live_bulkload, initial_count, 0, 0,
        deli_alex_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return deli_alex.query(query_case);
        }));
    rows.back().block_count = deli_alex.group_count();
    rows.back().tree_nodes = deli_alex.leaf_count();
    rows.back().stale_block_count = deli_alex.stale_leaf_count();
    rows.back().summary_rebuild_count =
        deli_alex.summary_rebuild_count() +
        deli_alex.group_directory_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex.summary_rebuild_ns() + deli_alex.group_directory_rebuild_ns();
    rows.back().index_bytes_estimate = deli_alex.index_bytes_estimate();
    rows.back().note =
        "DELI-ALEX uses existing ALEX leaves plus IntervalOverlap-style leaf-group summaries.";
    print_compare_summary(rows.back());

    auto deli_alex_insert_start = std::chrono::high_resolution_clock::now();
    std::size_t deli_alex_insert_success = 0;
    std::size_t deli_alex_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      if (deli_alex.insert(oid)) {
        ++deli_alex_insert_success;
      } else {
        ++deli_alex_insert_failed;
      }
    }
    auto deli_alex_insert_end = std::chrono::high_resolution_clock::now();
    long long deli_alex_insert_ns =
        ns_count(deli_alex_insert_end - deli_alex_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX", "after_insert", 1, geometries, queries,
        live_after_insert, live_insert, initial_count, insert_count, 0,
        deli_alex_build_ns, deli_alex_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return deli_alex.query(query_case);
        }));
    rows.back().success_count = deli_alex_insert_success;
    rows.back().failed_count = deli_alex_insert_failed;
    rows.back().block_count = deli_alex.group_count();
    rows.back().tree_nodes = deli_alex.leaf_count();
    rows.back().stale_block_count = deli_alex.stale_leaf_count();
    rows.back().summary_rebuild_count =
        deli_alex.summary_rebuild_count() +
        deli_alex.group_directory_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex.summary_rebuild_ns() + deli_alex.group_directory_rebuild_ns();
    rows.back().index_bytes_estimate = deli_alex.index_bytes_estimate();
    rows.back().note =
        "DELI-ALEX uses existing ALEX leaves plus IntervalOverlap-style leaf-group summaries.";
    print_compare_summary(rows.back());

    auto deli_alex_delete_start = std::chrono::high_resolution_clock::now();
    std::size_t deli_alex_delete_success = 0;
    std::size_t deli_alex_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      if (deli_alex.erase(oid)) {
        ++deli_alex_delete_success;
      } else {
        ++deli_alex_delete_failed;
      }
    }
    auto deli_alex_delete_end = std::chrono::high_resolution_clock::now();
    long long deli_alex_delete_ns =
        ns_count(deli_alex_delete_end - deli_alex_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX", "after_delete", 2, geometries, queries,
        live_after_delete, live_delete, initial_count, insert_count,
        delete_count, deli_alex_build_ns, deli_alex_insert_ns,
        deli_alex_delete_ns,
        [&](const QueryCase& query_case) {
          return deli_alex.query(query_case);
        }));
    rows.back().success_count = deli_alex_delete_success;
    rows.back().failed_count = deli_alex_delete_failed;
    rows.back().block_count = deli_alex.group_count();
    rows.back().tree_nodes = deli_alex.leaf_count();
    rows.back().stale_block_count = deli_alex.stale_leaf_count();
    rows.back().summary_rebuild_count =
        deli_alex.summary_rebuild_count() +
        deli_alex.group_directory_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex.summary_rebuild_ns() + deli_alex.group_directory_rebuild_ns();
    rows.back().index_bytes_estimate = deli_alex.index_bytes_estimate();
    rows.back().note =
        "DELI-ALEX uses existing ALEX leaves plus IntervalOverlap-style leaf-group summaries.";
    print_compare_summary(rows.back());
    }

    // DELI-ALEX-Hybrid: ALEX handles dynamic writes; a compact object-id
    // block overlay handles exact-query pruning and cache-friendly scans.
    if (index_enabled(options, "DELI_ALEX_HYBRID")) {
    DeliAlexHybridIndex deli_alex_hybrid(options, geometries,
                                         geometry_metadata);
    auto hybrid_build_start = std::chrono::high_resolution_clock::now();
    deli_alex_hybrid.bulk_load(initial_ids);
    auto hybrid_build_end = std::chrono::high_resolution_clock::now();
    long long hybrid_build_ns = ns_count(hybrid_build_end - hybrid_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID", "after_bulkload", 0, geometries, queries,
        live_after_bulkload, live_bulkload, initial_count, 0, 0,
        hybrid_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid.query(query_case);
        }));
    rows.back().block_count = deli_alex_hybrid.block_count();
    rows.back().tree_nodes = deli_alex_hybrid.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid.summary_rebuild_count();
    rows.back().summary_rebuild_ns = deli_alex_hybrid.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid.block_split_count();
    rows.back().index_bytes_estimate = deli_alex_hybrid.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid uses ALEX for writes plus a compact IntervalOverlap-style object-id overlay for queries.";
    print_compare_summary(rows.back());

    auto hybrid_insert_start = std::chrono::high_resolution_clock::now();
    std::size_t hybrid_insert_success = 0;
    std::size_t hybrid_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      if (deli_alex_hybrid.insert(oid)) {
        ++hybrid_insert_success;
      } else {
        ++hybrid_insert_failed;
      }
    }
    auto hybrid_insert_end = std::chrono::high_resolution_clock::now();
    long long hybrid_insert_ns =
        ns_count(hybrid_insert_end - hybrid_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID", "after_insert", 1, geometries, queries,
        live_after_insert, live_insert, initial_count, insert_count, 0,
        hybrid_build_ns, hybrid_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid.query(query_case);
        }));
    rows.back().success_count = hybrid_insert_success;
    rows.back().failed_count = hybrid_insert_failed;
    rows.back().block_count = deli_alex_hybrid.block_count();
    rows.back().tree_nodes = deli_alex_hybrid.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid.summary_rebuild_count();
    rows.back().summary_rebuild_ns = deli_alex_hybrid.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid.block_split_count();
    rows.back().index_bytes_estimate = deli_alex_hybrid.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid uses ALEX for writes plus a compact IntervalOverlap-style object-id overlay for queries.";
    print_compare_summary(rows.back());

    auto hybrid_delete_start = std::chrono::high_resolution_clock::now();
    std::size_t hybrid_delete_success = 0;
    std::size_t hybrid_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      if (deli_alex_hybrid.erase(oid)) {
        ++hybrid_delete_success;
      } else {
        ++hybrid_delete_failed;
      }
    }
    auto hybrid_delete_end = std::chrono::high_resolution_clock::now();
    long long hybrid_delete_ns =
        ns_count(hybrid_delete_end - hybrid_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID", "after_delete", 2, geometries, queries,
        live_after_delete, live_delete, initial_count, insert_count,
        delete_count, hybrid_build_ns, hybrid_insert_ns, hybrid_delete_ns,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid.query(query_case);
        }));
    rows.back().success_count = hybrid_delete_success;
    rows.back().failed_count = hybrid_delete_failed;
    rows.back().block_count = deli_alex_hybrid.block_count();
    rows.back().tree_nodes = deli_alex_hybrid.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid.summary_rebuild_count();
    rows.back().summary_rebuild_ns = deli_alex_hybrid.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid.block_split_count();
    rows.back().index_bytes_estimate = deli_alex_hybrid.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid uses ALEX for writes plus a compact IntervalOverlap-style object-id overlay for queries.";
    print_compare_summary(rows.back());
    }

    // DELI-ALEX-Hybrid-Buffered: same query overlay idea, but new inserts are
    // appended into small delta blocks before being queried as sorted segments.
    if (index_enabled(options, "DELI_ALEX_HYBRID_BUF")) {
    DeliAlexHybridBufferedIndex deli_alex_hybrid_buf(options, geometries,
                                                     geometry_metadata);
    auto hybrid_buf_build_start = std::chrono::high_resolution_clock::now();
    deli_alex_hybrid_buf.bulk_load(initial_ids);
    auto hybrid_buf_build_end = std::chrono::high_resolution_clock::now();
    long long hybrid_buf_build_ns =
        ns_count(hybrid_buf_build_end - hybrid_buf_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BUF", "after_bulkload", 0, geometries,
        queries, live_after_bulkload, live_bulkload, initial_count, 0, 0,
        hybrid_buf_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_buf.query(query_case);
        }));
    rows.back().block_count = deli_alex_hybrid_buf.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_buf.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid_buf.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_buf.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_buf.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid_buf.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_buf.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_buf.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BUF validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-Buf appends inserts into compact delta blocks to reduce foreground overlay maintenance.";
    print_compare_summary(rows.back());

    auto hybrid_buf_insert_start = std::chrono::high_resolution_clock::now();
    std::size_t hybrid_buf_insert_success = 0;
    std::size_t hybrid_buf_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      if (deli_alex_hybrid_buf.insert(oid)) {
        ++hybrid_buf_insert_success;
      } else {
        ++hybrid_buf_insert_failed;
      }
    }
    auto hybrid_buf_insert_end = std::chrono::high_resolution_clock::now();
    long long hybrid_buf_insert_ns =
        ns_count(hybrid_buf_insert_end - hybrid_buf_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BUF", "after_insert", 1, geometries,
        queries, live_after_insert, live_insert, initial_count, insert_count, 0,
        hybrid_buf_build_ns, hybrid_buf_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_buf.query(query_case);
        }));
    rows.back().success_count = hybrid_buf_insert_success;
    rows.back().failed_count = hybrid_buf_insert_failed;
    rows.back().block_count = deli_alex_hybrid_buf.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_buf.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid_buf.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_buf.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_buf.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid_buf.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_buf.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_buf.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BUF validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-Buf appends inserts into compact delta blocks to reduce foreground overlay maintenance.";
    print_compare_summary(rows.back());

    auto hybrid_buf_delete_start = std::chrono::high_resolution_clock::now();
    std::size_t hybrid_buf_delete_success = 0;
    std::size_t hybrid_buf_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      if (deli_alex_hybrid_buf.erase(oid)) {
        ++hybrid_buf_delete_success;
      } else {
        ++hybrid_buf_delete_failed;
      }
    }
    auto hybrid_buf_delete_end = std::chrono::high_resolution_clock::now();
    long long hybrid_buf_delete_ns =
        ns_count(hybrid_buf_delete_end - hybrid_buf_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BUF", "after_delete", 2, geometries,
        queries, live_after_delete, live_delete, initial_count, insert_count,
        delete_count, hybrid_buf_build_ns, hybrid_buf_insert_ns,
        hybrid_buf_delete_ns,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_buf.query(query_case);
        }));
    rows.back().success_count = hybrid_buf_delete_success;
    rows.back().failed_count = hybrid_buf_delete_failed;
    rows.back().block_count = deli_alex_hybrid_buf.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_buf.leaf_count();
    rows.back().stale_block_count = deli_alex_hybrid_buf.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_buf.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_buf.summary_rebuild_ns();
    rows.back().block_split_count = deli_alex_hybrid_buf.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_buf.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_buf.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BUF validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
      rows.back().note =
          "DELI-ALEX-Hybrid-Buf appends inserts into compact delta blocks to reduce foreground overlay maintenance.";
      print_compare_summary(rows.back());
      }

      // DELI-ALEX-Hybrid-Bounded: keep delta writes cheap, but periodically
      // promote bounded delta blocks into the ordered query directory.
      if (index_enabled(options, "DELI_ALEX_HYBRID_BOUNDED")) {
      DeliAlexHybridBufferedIndex deli_alex_hybrid_bounded(
          options, geometries, geometry_metadata, true);
    auto hybrid_bounded_build_start = std::chrono::high_resolution_clock::now();
    deli_alex_hybrid_bounded.bulk_load(initial_ids);
    auto hybrid_bounded_build_end = std::chrono::high_resolution_clock::now();
    long long hybrid_bounded_build_ns =
        ns_count(hybrid_bounded_build_end - hybrid_bounded_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BOUNDED", "after_bulkload", 0, geometries,
        queries, live_after_bulkload, live_bulkload, initial_count, 0, 0,
        hybrid_bounded_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_bounded.query(query_case);
        }));
    rows.back().block_count = deli_alex_hybrid_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_bounded.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_bounded.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-Bounded uses adaptive delta promotion; adaptive_bound_records=" +
        std::to_string(deli_alex_hybrid_bounded.adaptive_delta_record_bound());
    print_compare_summary(rows.back());

    auto hybrid_bounded_insert_start =
        std::chrono::high_resolution_clock::now();
    std::size_t hybrid_bounded_insert_success = 0;
    std::size_t hybrid_bounded_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      if (deli_alex_hybrid_bounded.insert(oid)) {
        ++hybrid_bounded_insert_success;
      } else {
        ++hybrid_bounded_insert_failed;
      }
    }
    auto hybrid_bounded_insert_end = std::chrono::high_resolution_clock::now();
    long long hybrid_bounded_insert_ns =
        ns_count(hybrid_bounded_insert_end - hybrid_bounded_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BOUNDED", "after_insert", 1, geometries,
        queries, live_after_insert, live_insert, initial_count, insert_count, 0,
        hybrid_bounded_build_ns, hybrid_bounded_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_bounded.query(query_case);
        }));
    rows.back().success_count = hybrid_bounded_insert_success;
    rows.back().failed_count = hybrid_bounded_insert_failed;
    rows.back().block_count = deli_alex_hybrid_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_bounded.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_bounded.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-Bounded uses adaptive delta promotion; adaptive_bound_records=" +
        std::to_string(deli_alex_hybrid_bounded.adaptive_delta_record_bound());
    print_compare_summary(rows.back());

    auto hybrid_bounded_delete_start =
        std::chrono::high_resolution_clock::now();
    std::size_t hybrid_bounded_delete_success = 0;
    std::size_t hybrid_bounded_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      if (deli_alex_hybrid_bounded.erase(oid)) {
        ++hybrid_bounded_delete_success;
      } else {
        ++hybrid_bounded_delete_failed;
      }
    }
    auto hybrid_bounded_delete_end = std::chrono::high_resolution_clock::now();
    long long hybrid_bounded_delete_ns =
        ns_count(hybrid_bounded_delete_end - hybrid_bounded_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_BOUNDED", "after_delete", 2, geometries,
        queries, live_after_delete, live_delete, initial_count, insert_count,
        delete_count, hybrid_bounded_build_ns, hybrid_bounded_insert_ns,
        hybrid_bounded_delete_ns,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_bounded.query(query_case);
        }));
    rows.back().success_count = hybrid_bounded_delete_success;
    rows.back().failed_count = hybrid_bounded_delete_failed;
    rows.back().block_count = deli_alex_hybrid_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_bounded.block_split_count();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_bounded.validate_index(&validation_message) ? 1 : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
      rows.back().note =
          "DELI-ALEX-Hybrid-Bounded uses adaptive delta promotion; adaptive_bound_records=" +
          std::to_string(deli_alex_hybrid_bounded.adaptive_delta_record_bound());
      print_compare_summary(rows.back());
      }

      // DELI-ALEX-Hybrid-LocalBounded: each compact query block owns a small
      // local delta. Compaction only rebuilds the affected block.
      if (index_enabled(options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED")) {
      DeliAlexHybridLocalBoundedIndex deli_alex_hybrid_local_bounded(
          options, geometries, geometry_metadata);
    auto hybrid_local_build_start = std::chrono::high_resolution_clock::now();
    deli_alex_hybrid_local_bounded.bulk_load(initial_ids);
    auto hybrid_local_build_end = std::chrono::high_resolution_clock::now();
    long long hybrid_local_build_ns =
        ns_count(hybrid_local_build_end - hybrid_local_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED", "after_bulkload", 0,
        geometries, queries, live_after_bulkload, live_bulkload, initial_count,
        0, 0, hybrid_local_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_local_bounded.query(query_case);
        }));
    rows.back().block_count = deli_alex_hybrid_local_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_local_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_local_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_local_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_local_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_local_bounded.block_split_count();
    rows.back().local_compaction_count =
        deli_alex_hybrid_local_bounded.local_compaction_count();
    rows.back().local_compaction_ns =
        deli_alex_hybrid_local_bounded.local_compaction_ns();
    rows.back().local_delta_bound =
        deli_alex_hybrid_local_bounded.local_delta_bound();
    rows.back().delete_compact_bound =
        deli_alex_hybrid_local_bounded.delete_compact_bound();
    rows.back().max_local_delta_size =
        deli_alex_hybrid_local_bounded.max_local_delta_size();
    rows.back().blocks_with_delta =
        deli_alex_hybrid_local_bounded.blocks_with_delta();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_local_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_local_bounded.validate_index(&validation_message)
              ? 1
              : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_LOCAL_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-LocalBounded uses per-block bounded delta and lazy tombstone compaction; local_delta_bound=" +
        std::to_string(deli_alex_hybrid_local_bounded.local_delta_bound()) +
        " delete_compact_bound=" +
        std::to_string(deli_alex_hybrid_local_bounded.delete_compact_bound());
    print_compare_summary(rows.back());

    auto hybrid_local_insert_start =
        std::chrono::high_resolution_clock::now();
    std::size_t hybrid_local_insert_success = 0;
    std::size_t hybrid_local_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      if (deli_alex_hybrid_local_bounded.insert(oid)) {
        ++hybrid_local_insert_success;
      } else {
        ++hybrid_local_insert_failed;
      }
    }
    auto hybrid_local_insert_end = std::chrono::high_resolution_clock::now();
    long long hybrid_local_insert_ns =
        ns_count(hybrid_local_insert_end - hybrid_local_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED", "after_insert", 1,
        geometries, queries, live_after_insert, live_insert, initial_count,
        insert_count, 0, hybrid_local_build_ns, hybrid_local_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_local_bounded.query(query_case);
        }));
    rows.back().success_count = hybrid_local_insert_success;
    rows.back().failed_count = hybrid_local_insert_failed;
    rows.back().block_count = deli_alex_hybrid_local_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_local_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_local_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_local_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_local_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_local_bounded.block_split_count();
    rows.back().local_compaction_count =
        deli_alex_hybrid_local_bounded.local_compaction_count();
    rows.back().local_compaction_ns =
        deli_alex_hybrid_local_bounded.local_compaction_ns();
    rows.back().local_delta_bound =
        deli_alex_hybrid_local_bounded.local_delta_bound();
    rows.back().delete_compact_bound =
        deli_alex_hybrid_local_bounded.delete_compact_bound();
    rows.back().max_local_delta_size =
        deli_alex_hybrid_local_bounded.max_local_delta_size();
    rows.back().blocks_with_delta =
        deli_alex_hybrid_local_bounded.blocks_with_delta();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_local_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_local_bounded.validate_index(&validation_message)
              ? 1
              : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_LOCAL_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-LocalBounded uses per-block bounded delta and lazy tombstone compaction; local_delta_bound=" +
        std::to_string(deli_alex_hybrid_local_bounded.local_delta_bound()) +
        " delete_compact_bound=" +
        std::to_string(deli_alex_hybrid_local_bounded.delete_compact_bound());
    print_compare_summary(rows.back());

    auto hybrid_local_delete_start =
        std::chrono::high_resolution_clock::now();
    std::size_t hybrid_local_delete_success = 0;
    std::size_t hybrid_local_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      if (deli_alex_hybrid_local_bounded.erase(oid)) {
        ++hybrid_local_delete_success;
      } else {
        ++hybrid_local_delete_failed;
      }
    }
    auto hybrid_local_delete_end = std::chrono::high_resolution_clock::now();
    long long hybrid_local_delete_ns =
        ns_count(hybrid_local_delete_end - hybrid_local_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "DELI_ALEX_HYBRID_LOCAL_BOUNDED", "after_delete", 2,
        geometries, queries, live_after_delete, live_delete, initial_count,
        insert_count, delete_count, hybrid_local_build_ns,
        hybrid_local_insert_ns, hybrid_local_delete_ns,
        [&](const QueryCase& query_case) {
          return deli_alex_hybrid_local_bounded.query(query_case);
        }));
    rows.back().success_count = hybrid_local_delete_success;
    rows.back().failed_count = hybrid_local_delete_failed;
    rows.back().block_count = deli_alex_hybrid_local_bounded.block_count();
    rows.back().tree_nodes = deli_alex_hybrid_local_bounded.leaf_count();
    rows.back().stale_block_count =
        deli_alex_hybrid_local_bounded.stale_block_count();
    rows.back().summary_rebuild_count =
        deli_alex_hybrid_local_bounded.summary_rebuild_count();
    rows.back().summary_rebuild_ns =
        deli_alex_hybrid_local_bounded.summary_rebuild_ns();
    rows.back().block_split_count =
        deli_alex_hybrid_local_bounded.block_split_count();
    rows.back().local_compaction_count =
        deli_alex_hybrid_local_bounded.local_compaction_count();
    rows.back().local_compaction_ns =
        deli_alex_hybrid_local_bounded.local_compaction_ns();
    rows.back().local_delta_bound =
        deli_alex_hybrid_local_bounded.local_delta_bound();
    rows.back().delete_compact_bound =
        deli_alex_hybrid_local_bounded.delete_compact_bound();
    rows.back().max_local_delta_size =
        deli_alex_hybrid_local_bounded.max_local_delta_size();
    rows.back().blocks_with_delta =
        deli_alex_hybrid_local_bounded.blocks_with_delta();
    rows.back().index_bytes_estimate =
        deli_alex_hybrid_local_bounded.index_bytes_estimate();
    {
      std::string validation_message;
      rows.back().validate_ok =
          deli_alex_hybrid_local_bounded.validate_index(&validation_message)
              ? 1
              : 0;
      if (!rows.back().validate_ok) {
        std::cerr << "DELI_ALEX_HYBRID_LOCAL_BOUNDED validate_index failed at "
                  << rows.back().checkpoint << ": " << validation_message
                  << "\n";
      }
    }
    rows.back().note =
        "DELI-ALEX-Hybrid-LocalBounded uses per-block bounded delta and lazy tombstone compaction; local_delta_bound=" +
        std::to_string(deli_alex_hybrid_local_bounded.local_delta_bound()) +
        " delete_compact_bound=" +
          std::to_string(deli_alex_hybrid_local_bounded.delete_compact_bound());
      print_compare_summary(rows.back());
      }

      // Boost R-tree.
      if (index_enabled(options, "Boost_Rtree")) {
      auto boost_build_start = std::chrono::high_resolution_clock::now();
      RTree boost_index =
        build_rtree_for_ids(geometries, initial_ids, live_bulkload, nullptr);
    auto boost_build_end = std::chrono::high_resolution_clock::now();
    long long boost_build_ns = ns_count(boost_build_end - boost_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "Boost_Rtree", "after_bulkload", 0, geometries, queries,
        live_after_bulkload, live_bulkload, initial_count, 0, 0,
        boost_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return query_boost_index(boost_index, geometries, live_bulkload,
                                   query_case);
        }));
    rows.back().index_bytes_estimate =
        estimate_boost_bytes(live_after_bulkload.size());
    rows.back().note = "Boost bytes are rough entry/node estimates.";
    print_compare_summary(rows.back());

    auto boost_insert_start = std::chrono::high_resolution_clock::now();
    for (ObjectId oid : insert_ids) {
      boost_index.insert(std::make_pair(
          boost_box_from_envelope(*geometries[oid]->getEnvelopeInternal()),
          oid));
    }
    auto boost_insert_end = std::chrono::high_resolution_clock::now();
    long long boost_insert_ns = ns_count(boost_insert_end - boost_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "Boost_Rtree", "after_insert", 1, geometries, queries,
        live_after_insert, live_insert, initial_count, insert_count, 0,
        boost_build_ns, boost_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return query_boost_index(boost_index, geometries, live_insert,
                                   query_case);
        }));
    rows.back().index_bytes_estimate =
        estimate_boost_bytes(live_after_insert.size());
    rows.back().note = "Boost bytes are rough entry/node estimates.";
    print_compare_summary(rows.back());

    std::size_t boost_delete_success = 0;
    std::size_t boost_delete_failed = 0;
    auto boost_delete_start = std::chrono::high_resolution_clock::now();
    for (ObjectId oid : delete_ids) {
      std::size_t removed = boost_index.remove(std::make_pair(
          boost_box_from_envelope(*geometries[oid]->getEnvelopeInternal()),
          oid));
      if (removed > 0) {
        ++boost_delete_success;
      } else {
        ++boost_delete_failed;
      }
    }
    auto boost_delete_end = std::chrono::high_resolution_clock::now();
    long long boost_delete_ns = ns_count(boost_delete_end - boost_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "Boost_Rtree", "after_delete", 2, geometries, queries,
        live_after_delete, live_delete, initial_count, insert_count,
        delete_count, boost_build_ns, boost_insert_ns, boost_delete_ns,
        [&](const QueryCase& query_case) {
          return query_boost_index(boost_index, geometries, live_delete,
                                   query_case);
        }));
    rows.back().success_count = boost_delete_success;
    rows.back().failed_count = boost_delete_failed;
      rows.back().index_bytes_estimate =
          estimate_boost_bytes(live_after_delete.size());
      rows.back().note = "Boost bytes are rough entry/node estimates.";
      print_compare_summary(rows.back());
      }

      // GEOS Quadtree.
      if (index_enabled(options, "GEOS_Quadtree")) {
      Quadtree quadtree;
      auto quad_build_start = std::chrono::high_resolution_clock::now();
    for (ObjectId oid : initial_ids) {
      Geometry* geometry = geometries[oid].get();
      quadtree.insert(geometry->getEnvelopeInternal(), geometry);
    }
    auto quad_build_end = std::chrono::high_resolution_clock::now();
    long long quad_build_ns = ns_count(quad_build_end - quad_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "GEOS_Quadtree", "after_bulkload", 0, geometries, queries,
        live_after_bulkload, live_bulkload, initial_count, 0, 0,
        quad_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return query_quadtree_index(quadtree, geometry_to_oid, geometries,
                                      live_bulkload, query_case);
        }));
    rows.back().tree_size = quadtree.size();
    rows.back().tree_depth = static_cast<std::size_t>(quadtree.depth());
    rows.back().tree_nodes = 0;
    rows.back().index_bytes_estimate = estimate_quadtree_bytes(quadtree);
    rows.back().note = "Quadtree bytes are rough node/item estimates.";
    print_compare_summary(rows.back());

    auto quad_insert_start = std::chrono::high_resolution_clock::now();
    for (ObjectId oid : insert_ids) {
      Geometry* geometry = geometries[oid].get();
      quadtree.insert(geometry->getEnvelopeInternal(), geometry);
    }
    auto quad_insert_end = std::chrono::high_resolution_clock::now();
    long long quad_insert_ns = ns_count(quad_insert_end - quad_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "GEOS_Quadtree", "after_insert", 1, geometries, queries,
        live_after_insert, live_insert, initial_count, insert_count, 0,
        quad_build_ns, quad_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return query_quadtree_index(quadtree, geometry_to_oid, geometries,
                                      live_insert, query_case);
        }));
    rows.back().tree_size = quadtree.size();
    rows.back().tree_depth = static_cast<std::size_t>(quadtree.depth());
    rows.back().tree_nodes = 0;
    rows.back().index_bytes_estimate = estimate_quadtree_bytes(quadtree);
    rows.back().note = "Quadtree bytes are rough node/item estimates.";
    print_compare_summary(rows.back());

    std::size_t quad_delete_success = 0;
    std::size_t quad_delete_failed = 0;
    auto quad_delete_start = std::chrono::high_resolution_clock::now();
    for (ObjectId oid : delete_ids) {
      Geometry* geometry = geometries[oid].get();
      if (quadtree.remove(geometry->getEnvelopeInternal(), geometry)) {
        ++quad_delete_success;
      } else {
        ++quad_delete_failed;
      }
    }
    auto quad_delete_end = std::chrono::high_resolution_clock::now();
    long long quad_delete_ns = ns_count(quad_delete_end - quad_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "GEOS_Quadtree", "after_delete", 2, geometries, queries,
        live_after_delete, live_delete, initial_count, insert_count,
        delete_count, quad_build_ns, quad_insert_ns, quad_delete_ns,
        [&](const QueryCase& query_case) {
          return query_quadtree_index(quadtree, geometry_to_oid, geometries,
                                      live_delete, query_case);
        }));
    rows.back().success_count = quad_delete_success;
    rows.back().failed_count = quad_delete_failed;
    rows.back().tree_size = quadtree.size();
    rows.back().tree_depth = static_cast<std::size_t>(quadtree.depth());
    rows.back().tree_nodes = 0;
      rows.back().index_bytes_estimate = estimate_quadtree_bytes(quadtree);
      rows.back().note = "Quadtree bytes are rough node/item estimates.";
      print_compare_summary(rows.back());
      }

      // GLIN-piece.
      if (index_enabled(options, "GLIN_PIECEWISE")) {
      alex::Glin<double, Geometry*> glin_piece;
      std::vector<std::tuple<double, double, double, double>> pieces;
    std::vector<Geometry*> glin_initial = raw_ptrs_for_ids(geometries, initial_ids);
    auto glin_build_start = std::chrono::high_resolution_clock::now();
    glin_piece.glin_bulk_load(glin_initial, options.piece_limit, "z",
                              options.cell_xmin, options.cell_ymin,
                              options.cell_size, options.cell_size, pieces);
    auto glin_build_end = std::chrono::high_resolution_clock::now();
    long long glin_build_ns = ns_count(glin_build_end - glin_build_start);
    rows.push_back(run_generic_checkpoint(
        options, "GLIN_PIECEWISE", "after_bulkload", 0, geometries, queries,
        live_after_bulkload, live_bulkload, initial_count, 0, 0,
        glin_build_ns, 0, 0,
        [&](const QueryCase& query_case) {
          return query_glin_piece_index(glin_piece, pieces, geometry_to_oid,
                                        live_bulkload, options, query_case);
        }));
    rows.back().pieces = pieces.size();
    rows.back().index_bytes_estimate =
        estimate_glin_piece_bytes(live_after_bulkload.size(), pieces.size());
    rows.back().note = "GLIN-piece bytes are rough payload/piece estimates.";
    print_compare_summary(rows.back());

    auto glin_insert_start = std::chrono::high_resolution_clock::now();
    std::size_t glin_insert_success = 0;
    std::size_t glin_insert_failed = 0;
    for (ObjectId oid : insert_ids) {
      Geometry* geometry = geometries[oid].get();
      geos::geom::Envelope* envelope =
          const_cast<geos::geom::Envelope*>(geometry->getEnvelopeInternal());
      auto result = glin_piece.glin_insert(
          std::make_tuple(geometry, envelope), "z", options.cell_xmin,
          options.cell_ymin, options.cell_size, options.cell_size,
          options.piece_limit, pieces);
      if (result.second) {
        ++glin_insert_success;
      } else {
        ++glin_insert_failed;
      }
    }
    auto glin_insert_end = std::chrono::high_resolution_clock::now();
    long long glin_insert_ns = ns_count(glin_insert_end - glin_insert_start);
    rows.push_back(run_generic_checkpoint(
        options, "GLIN_PIECEWISE", "after_insert", 1, geometries, queries,
        live_after_insert, live_insert, initial_count, insert_count, 0,
        glin_build_ns, glin_insert_ns, 0,
        [&](const QueryCase& query_case) {
          return query_glin_piece_index(glin_piece, pieces, geometry_to_oid,
                                        live_insert, options, query_case);
        }));
    rows.back().success_count = glin_insert_success;
    rows.back().failed_count = glin_insert_failed;
    rows.back().pieces = pieces.size();
    rows.back().index_bytes_estimate =
        estimate_glin_piece_bytes(live_after_insert.size(), pieces.size());
    rows.back().note = "GLIN-piece bytes are rough payload/piece estimates.";
    print_compare_summary(rows.back());

    auto glin_delete_start = std::chrono::high_resolution_clock::now();
    std::size_t glin_delete_success = 0;
    std::size_t glin_delete_failed = 0;
    for (ObjectId oid : delete_ids) {
      Geometry* geometry = geometries[oid].get();
      int erased = glin_piece.erase(geometry, "z", options.cell_xmin,
                                    options.cell_ymin, options.cell_size,
                                    options.cell_size, options.piece_limit,
                                    pieces);
      if (erased > 0) {
        ++glin_delete_success;
      } else {
        ++glin_delete_failed;
      }
    }
    auto glin_delete_end = std::chrono::high_resolution_clock::now();
    long long glin_delete_ns = ns_count(glin_delete_end - glin_delete_start);
    rows.push_back(run_generic_checkpoint(
        options, "GLIN_PIECEWISE", "after_delete", 2, geometries, queries,
        live_after_delete, live_delete, initial_count, insert_count,
        delete_count, glin_build_ns, glin_insert_ns, glin_delete_ns,
        [&](const QueryCase& query_case) {
          return query_glin_piece_index(glin_piece, pieces, geometry_to_oid,
                                        live_delete, options, query_case);
        }));
    rows.back().success_count = glin_delete_success;
    rows.back().failed_count = glin_delete_failed;
    rows.back().pieces = pieces.size();
      rows.back().index_bytes_estimate =
          estimate_glin_piece_bytes(live_after_delete.size(), pieces.size());
      rows.back().note = "GLIN-piece bytes are rough payload/piece estimates.";
      print_compare_summary(rows.back());
      }

      annotate_stage_maintenance(rows);
    write_compare_csv(options.output_csv, options, stats, rows);
    if (!options.output_csv.empty()) {
      std::cout << "CSV: " << options.output_csv << "\n";
    }
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }
  return 0;
}
