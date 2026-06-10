// Interval-overlap prototype for GLIN Intersects.
//
// This benchmark intentionally stays independent from glin/glin.h. It validates
// the core idea first: sort geometries by Zmin, summarize fixed-size blocks by
// maxZmax and MBR, then safely skip blocks that cannot overlap a query interval.

#include "../../src/core/projection.h"

#include <geos/geom/Coordinate.h>
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/LinearRing.h>
#include <geos/geom/Polygon.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;
using BoostPoint = bg::model::d2::point_xy<double>;
using BoostBox = bg::model::box<BoostPoint>;
using RTreeValue = std::pair<BoostBox, std::size_t>;

struct Options {
  std::string data_file;
  std::string dataset_name = "WKT";
  std::string query_file;
  std::string output_csv;
  std::size_t limit = 10000;
  std::size_t query_count = 20;
  std::size_t block_size = 1024;
  std::uint64_t seed = 42;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  std::string variant = "block_mbr";
  double overflow_fraction = 0.01;
  bool enable_zmax_skip = true;
  bool enable_block_mbr_skip = true;
  bool enable_record_mbr_filter = true;
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

struct IntervalRecord {
  double zmin = 0.0;
  double zmax = 0.0;
  geos::geom::Envelope envelope;
  Geometry* geometry = nullptr;
  std::size_t id = 0;
};

struct IntervalBlock {
  std::size_t begin = 0;
  std::size_t end = 0;
  double min_zmin = 0.0;
  double max_zmin = 0.0;
  double max_zmax = 0.0;
  geos::geom::Envelope mbr;
};

struct IntervalIndex {
  std::vector<IntervalRecord> records;
  std::vector<IntervalBlock> blocks;
  std::vector<double> zmins;
  std::vector<IntervalRecord> overflow_records;
  bgi::rtree<RTreeValue, bgi::quadratic<16>> overflow_rtree;
  double overflow_span_threshold = std::numeric_limits<double>::infinity();
};

struct QueryResult {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  long long probe_ns = 0;
  long long refine_ns = 0;
  long long total_ns = 0;
  std::size_t prefix_records = 0;
  std::size_t prefix_blocks = 0;
  std::size_t visited_blocks = 0;
  std::size_t skipped_zmax_blocks = 0;
  std::size_t skipped_mbr_blocks = 0;
  std::size_t records_scanned = 0;
  std::size_t interval_candidates = 0;
  std::size_t mbr_candidates = 0;
  std::size_t exact_calls = 0;
  std::size_t answers = 0;
  std::size_t overflow_probe_candidates = 0;
  std::size_t overflow_interval_candidates = 0;
  std::size_t overflow_exact_calls = 0;
  std::size_t overflow_answers = 0;
};

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file /path/to/data.wkt [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME          Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N                    Number of valid geometries to load (default: 10000)\n"
      << "  --query_file PATH            Query CSV: query_id,xmin,ymin,xmax,ymax,source_geometry_id\n"
      << "  --query_count N              Random query count when no query_file is set (default: 20)\n"
      << "  --block_size N               Records per interval block (default: 1024)\n"
      << "  --seed N                     Random seed (default: 42)\n"
      << "  --cell_xmin X                Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y                Z-order latitude origin (default: -90)\n"
      << "  --cell_size S                Z-order cell size for x/y (default: 5e-7)\n"
      << "  --variant block_mbr|overflow Index variant (default: block_mbr)\n"
      << "  --overflow_fraction F        Top span fraction sent to overflow when variant=overflow (default: 0.01)\n"
      << "  --disable_zmax_skip 0|1      Disable block maxZmax skip when 1 (default: 0)\n"
      << "  --disable_block_mbr_skip 0|1 Disable block MBR skip when 1 (default: 0)\n"
      << "  --disable_record_mbr_filter 0|1 Disable record MBR filter when 1 (default: 0)\n"
      << "  --output_csv PATH            Write per-query CSV rows\n";
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
    } else if (key == "--block_size") {
      options.block_size =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--cell_xmin") {
      options.cell_xmin = std::stod(require_value(key));
    } else if (key == "--cell_ymin") {
      options.cell_ymin = std::stod(require_value(key));
    } else if (key == "--cell_size") {
      options.cell_size = std::stod(require_value(key));
    } else if (key == "--variant") {
      options.variant = require_value(key);
    } else if (key == "--overflow_fraction") {
      options.overflow_fraction = std::stod(require_value(key));
    } else if (key == "--disable_zmax_skip") {
      options.enable_zmax_skip = std::stoi(require_value(key)) == 0;
    } else if (key == "--disable_block_mbr_skip") {
      options.enable_block_mbr_skip = std::stoi(require_value(key)) == 0;
    } else if (key == "--disable_record_mbr_filter") {
      options.enable_record_mbr_filter = std::stoi(require_value(key)) == 0;
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
  if (options.query_count == 0) {
    throw std::runtime_error("--query_count must be greater than 0");
  }
  if (options.block_size == 0) {
    throw std::runtime_error("--block_size must be greater than 0");
  }
  if (options.variant != "block_mbr" && options.variant != "overflow") {
    throw std::runtime_error("--variant must be block_mbr or overflow");
  }
  if (options.overflow_fraction < 0.0 || options.overflow_fraction >= 1.0) {
    throw std::runtime_error("--overflow_fraction must be in [0, 1)");
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

BoostBox boost_box_from_envelope(const geos::geom::Envelope& envelope) {
  return BoostBox(BoostPoint(envelope.getMinX(), envelope.getMinY()),
                  BoostPoint(envelope.getMaxX(), envelope.getMaxY()));
}

std::vector<Geometry*> raw_ptrs(const std::vector<GeometryPtr>& geometries) {
  std::vector<Geometry*> raw;
  raw.reserve(geometries.size());
  for (const auto& geometry : geometries) {
    raw.push_back(geometry.get());
  }
  return raw;
}

GeometryPtr make_query_box(geos::geom::GeometryFactory& factory,
                           double xmin, double ymin, double xmax, double ymax) {
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
    const std::string& query_file, geos::geom::GeometryFactory& factory,
    std::vector<GeometryPtr>& owned_queries) {
  std::ifstream input(query_file);
  if (!input) {
    throw std::runtime_error("Cannot open query file: " + query_file);
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
  }

  if (queries.empty()) {
    throw std::runtime_error("No query rows loaded from: " + query_file);
  }
  return queries;
}

std::vector<QueryCase> make_random_queries(const std::vector<Geometry*>& geometries,
                                           std::size_t query_count,
                                           std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> distribution(0,
                                                          geometries.size() - 1);
  std::vector<QueryCase> queries;
  queries.reserve(query_count);
  for (std::size_t i = 0; i < query_count; ++i) {
    std::size_t id = distribution(rng);
    QueryCase query;
    query.query_id = i;
    query.source_geometry_id = id;
    query.geometry = geometries[id];
    const geos::geom::Envelope* env = query.geometry->getEnvelopeInternal();
    query.xmin = env->getMinX();
    query.ymin = env->getMinY();
    query.xmax = env->getMaxX();
    query.ymax = env->getMaxY();
    queries.push_back(query);
  }
  return queries;
}

IntervalIndex build_interval_index(const Options& options,
                                   const std::vector<GeometryPtr>& geometries) {
  IntervalIndex index;
  std::vector<IntervalRecord> all_records;
  all_records.reserve(geometries.size());

  for (std::size_t i = 0; i < geometries.size(); ++i) {
    Geometry* geometry = geometries[i].get();
    double zmin = 0.0;
    double zmax = 0.0;
    curve_shape_projection(geometry, "z", options.cell_xmin, options.cell_ymin,
                           options.cell_size, options.cell_size, zmin, zmax);

    IntervalRecord record;
    record.zmin = zmin;
    record.zmax = zmax;
    record.envelope = *geometry->getEnvelopeInternal();
    record.geometry = geometry;
    record.id = i;
    all_records.push_back(record);
  }

  if (options.variant == "overflow" && options.overflow_fraction > 0.0 &&
      !all_records.empty()) {
    std::size_t overflow_count = static_cast<std::size_t>(
        std::ceil(options.overflow_fraction * all_records.size()));
    overflow_count = std::min(overflow_count, all_records.size());
    if (overflow_count > 0) {
      std::vector<std::pair<double, std::size_t>> spans;
      spans.reserve(all_records.size());
      for (std::size_t i = 0; i < all_records.size(); ++i) {
        spans.emplace_back(all_records[i].zmax - all_records[i].zmin, i);
      }
      std::sort(spans.begin(), spans.end(),
                [](const auto& a, const auto& b) {
                  if (a.first != b.first) {
                    return a.first > b.first;
                  }
                  return a.second < b.second;
                });
      index.overflow_span_threshold = spans[overflow_count - 1].first;
      std::vector<char> is_overflow(all_records.size(), 0);
      for (std::size_t i = 0; i < overflow_count; ++i) {
        is_overflow[spans[i].second] = 1;
      }
      index.records.reserve(all_records.size() - overflow_count);
      index.overflow_records.reserve(overflow_count);
      for (std::size_t i = 0; i < all_records.size(); ++i) {
        if (is_overflow[i]) {
          index.overflow_records.push_back(all_records[i]);
        } else {
          index.records.push_back(all_records[i]);
        }
      }
    }
  }

  if (index.records.empty() && index.overflow_records.empty()) {
    index.records = all_records;
  }

  std::sort(index.records.begin(), index.records.end(),
            [](const IntervalRecord& a, const IntervalRecord& b) {
              if (a.zmin != b.zmin) {
                return a.zmin < b.zmin;
              }
              return a.id < b.id;
            });

  index.zmins.reserve(index.records.size());
  for (const auto& record : index.records) {
    index.zmins.push_back(record.zmin);
  }

  for (std::size_t begin = 0; begin < index.records.size();
       begin += options.block_size) {
    std::size_t end = std::min(begin + options.block_size,
                               index.records.size());
    IntervalBlock block;
    block.begin = begin;
    block.end = end;
    block.min_zmin = index.records[begin].zmin;
    block.max_zmin = index.records[end - 1].zmin;
    block.max_zmax = -std::numeric_limits<double>::infinity();
    block.mbr = index.records[begin].envelope;
    for (std::size_t i = begin; i < end; ++i) {
      block.max_zmax = std::max(block.max_zmax, index.records[i].zmax);
      block.mbr.expandToInclude(&index.records[i].envelope);
    }
    index.blocks.push_back(block);
  }

  std::vector<RTreeValue> overflow_values;
  overflow_values.reserve(index.overflow_records.size());
  for (std::size_t i = 0; i < index.overflow_records.size(); ++i) {
    overflow_values.emplace_back(
        boost_box_from_envelope(index.overflow_records[i].envelope), i);
  }
  if (!overflow_values.empty()) {
    index.overflow_rtree =
        bgi::rtree<RTreeValue, bgi::quadratic<16>>(overflow_values);
  }

  return index;
}

QueryResult query_interval_index(const Options& options,
                                 const IntervalIndex& index,
                                 const QueryCase& query) {
  QueryResult result;
  result.query_id = query.query_id;
  result.source_geometry_id = query.source_geometry_id;

  double query_zmin = 0.0;
  double query_zmax = 0.0;
  auto probe_start = std::chrono::high_resolution_clock::now();
  curve_shape_projection(query.geometry, "z", options.cell_xmin,
                         options.cell_ymin, options.cell_size,
                         options.cell_size, query_zmin, query_zmax);
  auto upper = std::upper_bound(index.zmins.begin(), index.zmins.end(),
                                query_zmax);
  result.prefix_records =
      static_cast<std::size_t>(upper - index.zmins.begin());
  result.prefix_blocks =
      (result.prefix_records + options.block_size - 1) / options.block_size;
  result.prefix_blocks = std::min(result.prefix_blocks, index.blocks.size());
  auto probe_end = std::chrono::high_resolution_clock::now();
  result.probe_ns = ns_count(probe_end - probe_start);

  geos::geom::Envelope query_envelope = *query.geometry->getEnvelopeInternal();

  auto refine_start = std::chrono::high_resolution_clock::now();
  for (std::size_t block_id = 0; block_id < result.prefix_blocks; ++block_id) {
    const IntervalBlock& block = index.blocks[block_id];
    std::size_t scan_end = std::min(block.end, result.prefix_records);
    if (scan_end <= block.begin) {
      continue;
    }

    if (options.enable_zmax_skip && block.max_zmax < query_zmin) {
      ++result.skipped_zmax_blocks;
      continue;
    }
    if (options.enable_block_mbr_skip &&
        !envelopes_intersect(block.mbr, query_envelope)) {
      ++result.skipped_mbr_blocks;
      continue;
    }

    ++result.visited_blocks;
    for (std::size_t i = block.begin; i < scan_end; ++i) {
      const IntervalRecord& record = index.records[i];
      if (record.zmin > query_zmax) {
        break;
      }
      ++result.records_scanned;
      if (record.zmax < query_zmin) {
        continue;
      }
      ++result.interval_candidates;
      if (options.enable_record_mbr_filter &&
          !envelopes_intersect(record.envelope, query_envelope)) {
        continue;
      }
      ++result.mbr_candidates;
      ++result.exact_calls;
      if (query.geometry->intersects(record.geometry)) {
        ++result.answers;
      }
    }
  }

  if (!index.overflow_records.empty()) {
    std::vector<RTreeValue> overflow_hits;
    index.overflow_rtree.query(
        bgi::intersects(boost_box_from_envelope(query_envelope)),
        std::back_inserter(overflow_hits));
    result.overflow_probe_candidates = overflow_hits.size();
    for (const auto& hit : overflow_hits) {
      const IntervalRecord& record = index.overflow_records[hit.second];
      if (record.zmin > query_zmax || record.zmax < query_zmin) {
        continue;
      }
      ++result.overflow_interval_candidates;
      ++result.overflow_exact_calls;
      ++result.exact_calls;
      if (query.geometry->intersects(record.geometry)) {
        ++result.overflow_answers;
        ++result.answers;
      }
    }
  }

  auto refine_end = std::chrono::high_resolution_clock::now();
  result.refine_ns = ns_count(refine_end - refine_start);
  result.total_ns = result.probe_ns + result.refine_ns;
  return result;
}

void write_csv(const std::string& path, const Options& options,
               const LoadStats& stats, std::size_t loaded_count,
               long long load_ns, long long build_ns,
               const IntervalIndex& index,
               const std::vector<QueryResult>& results) {
  if (path.empty()) {
    return;
  }

  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }

  output
      << "dataset,index,relationship,loaded_count,block_size,block_count,"
         "overflow_count,overflow_fraction,overflow_span_threshold,"
         "cell_xmin,cell_ymin,cell_size,seed,lines_seen,parse_errors,load_ns,"
         "build_ns,enable_zmax_skip,enable_block_mbr_skip,"
         "enable_record_mbr_filter,query_id,source_geometry_id,probe_ns,"
         "refine_ns,total_ns,prefix_records,prefix_blocks,visited_blocks,"
         "skipped_zmax_blocks,skipped_mbr_blocks,records_scanned,"
         "interval_candidates,mbr_candidates,exact_calls,answers,"
         "overflow_probe_candidates,overflow_interval_candidates,"
         "overflow_exact_calls,overflow_answers,candidate_answer_ratio\n";

  const char* index_name =
      options.variant == "overflow" ? "IO_OVERFLOW" : "IO_BLOCK_MBR";

  for (const auto& result : results) {
    double candidate_answer_ratio =
        result.answers == 0
            ? 0.0
            : static_cast<double>(result.exact_calls) /
                  static_cast<double>(result.answers);
    output << options.dataset_name << "," << index_name << ",intersects,"
           << loaded_count << "," << options.block_size << ","
           << index.blocks.size() << "," << index.overflow_records.size()
           << "," << options.overflow_fraction << ","
           << index.overflow_span_threshold << "," << options.cell_xmin << ","
           << options.cell_ymin << "," << options.cell_size << ","
           << options.seed << "," << stats.lines_seen << ","
           << stats.parse_errors << "," << load_ns << "," << build_ns << ","
           << (options.enable_zmax_skip ? 1 : 0) << ","
           << (options.enable_block_mbr_skip ? 1 : 0) << ","
           << (options.enable_record_mbr_filter ? 1 : 0) << ","
           << result.query_id << "," << result.source_geometry_id << ","
           << result.probe_ns << "," << result.refine_ns << ","
           << result.total_ns << "," << result.prefix_records << ","
           << result.prefix_blocks << "," << result.visited_blocks << ","
           << result.skipped_zmax_blocks << ","
           << result.skipped_mbr_blocks << "," << result.records_scanned << ","
           << result.interval_candidates << "," << result.mbr_candidates << ","
           << result.exact_calls << "," << result.answers << ","
           << result.overflow_probe_candidates << ","
           << result.overflow_interval_candidates << ","
           << result.overflow_exact_calls << "," << result.overflow_answers
           << ","
           << candidate_answer_ratio << "\n";
  }
}

void print_summary(const Options& options, const LoadStats& stats,
                   std::size_t loaded_count, long long load_ns,
                   long long build_ns, const IntervalIndex& index,
                   const std::vector<QueryResult>& results) {
  long long total_probe_ns = 0;
  long long total_refine_ns = 0;
  std::size_t total_prefix_records = 0;
  std::size_t total_prefix_blocks = 0;
  std::size_t total_visited_blocks = 0;
  std::size_t total_skipped_zmax_blocks = 0;
  std::size_t total_skipped_mbr_blocks = 0;
  std::size_t total_records_scanned = 0;
  std::size_t total_interval_candidates = 0;
  std::size_t total_mbr_candidates = 0;
  std::size_t total_exact_calls = 0;
  std::size_t total_answers = 0;
  std::size_t total_overflow_probe_candidates = 0;
  std::size_t total_overflow_interval_candidates = 0;
  std::size_t total_overflow_exact_calls = 0;
  std::size_t total_overflow_answers = 0;

  for (const auto& result : results) {
    total_probe_ns += result.probe_ns;
    total_refine_ns += result.refine_ns;
    total_prefix_records += result.prefix_records;
    total_prefix_blocks += result.prefix_blocks;
    total_visited_blocks += result.visited_blocks;
    total_skipped_zmax_blocks += result.skipped_zmax_blocks;
    total_skipped_mbr_blocks += result.skipped_mbr_blocks;
    total_records_scanned += result.records_scanned;
    total_interval_candidates += result.interval_candidates;
    total_mbr_candidates += result.mbr_candidates;
    total_exact_calls += result.exact_calls;
    total_answers += result.answers;
    total_overflow_probe_candidates += result.overflow_probe_candidates;
    total_overflow_interval_candidates += result.overflow_interval_candidates;
    total_overflow_exact_calls += result.overflow_exact_calls;
    total_overflow_answers += result.overflow_answers;
  }

  double candidate_answer_ratio =
      total_answers == 0 ? 0.0
                         : static_cast<double>(total_exact_calls) /
                               static_cast<double>(total_answers);
  double skipped_block_ratio =
      total_prefix_blocks == 0
          ? 0.0
          : static_cast<double>(total_skipped_zmax_blocks +
                                total_skipped_mbr_blocks) /
                static_cast<double>(total_prefix_blocks);

  const char* index_name =
      options.variant == "overflow" ? "IO_OVERFLOW" : "IO_BLOCK_MBR";
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "dataset=" << options.dataset_name
            << " index=" << index_name
            << " relationship=intersects"
            << " loaded=" << loaded_count
            << " blocks=" << index.blocks.size()
            << " block_size=" << options.block_size
            << " overflow_count=" << index.overflow_records.size()
            << " overflow_fraction=" << options.overflow_fraction
            << " overflow_span_threshold=" << index.overflow_span_threshold
            << " queries=" << results.size()
            << " lines_seen=" << stats.lines_seen
            << " parse_errors=" << stats.parse_errors
            << " load_ms=" << load_ns / 1e6
            << " build_ms=" << build_ns / 1e6
            << " avg_probe_ns="
            << (results.empty() ? 0.0
                                : static_cast<double>(total_probe_ns) /
                                      static_cast<double>(results.size()))
            << " avg_refine_ns="
            << (results.empty() ? 0.0
                                : static_cast<double>(total_refine_ns) /
                                      static_cast<double>(results.size()))
            << " prefix_records=" << total_prefix_records
            << " visited_blocks=" << total_visited_blocks
            << " skipped_zmax_blocks=" << total_skipped_zmax_blocks
            << " skipped_mbr_blocks=" << total_skipped_mbr_blocks
            << " skipped_block_ratio=" << skipped_block_ratio
            << " records_scanned=" << total_records_scanned
            << " interval_candidates=" << total_interval_candidates
            << " mbr_candidates=" << total_mbr_candidates
            << " exact_calls=" << total_exact_calls
            << " answers=" << total_answers
            << " overflow_probe_candidates="
            << total_overflow_probe_candidates
            << " overflow_interval_candidates="
            << total_overflow_interval_candidates
            << " overflow_exact_calls=" << total_overflow_exact_calls
            << " overflow_answers=" << total_overflow_answers
            << " candidate_answer_ratio=" << candidate_answer_ratio << "\n";
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
    long long load_ns = ns_count(load_end - load_start);

    if (geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }

    auto build_start = std::chrono::high_resolution_clock::now();
    IntervalIndex index = build_interval_index(options, geometries);
    auto build_end = std::chrono::high_resolution_clock::now();
    long long build_ns = ns_count(build_end - build_start);

    std::vector<Geometry*> raw = raw_ptrs(geometries);
    std::vector<GeometryPtr> owned_queries;
    std::vector<QueryCase> queries =
        options.query_file.empty()
            ? make_random_queries(raw, options.query_count, options.seed)
            : load_query_file(options.query_file, *factory, owned_queries);

    std::vector<QueryResult> results;
    results.reserve(queries.size());
    for (const auto& query : queries) {
      results.push_back(query_interval_index(options, index, query));
    }

    write_csv(options.output_csv, options, stats, geometries.size(), load_ns,
              build_ns, index, results);
    print_summary(options, stats, geometries.size(), load_ns, build_ns, index,
                  results);
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }
  return 0;
}
