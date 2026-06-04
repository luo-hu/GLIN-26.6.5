// Build GLIN, Boost R-tree, and GEOS Quadtree on one WKT dataset and report
// index-size estimates for Fig. 8.  Geometry object memory is deliberately
// excluded; the reported bytes describe the index structures only.

#include "../../glin/glin.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

// Boost R-tree and GEOS Quadtree do not expose all node types/counts through
// public APIs.  This benchmark only reads metadata, so it temporarily opens
// private members in this translation unit.
#define private public
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/geometry/index/detail/rtree/utilities/view.hpp>
#undef private

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#define private public
#include <geos/index/quadtree/Quadtree.h>
#undef private

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
  std::string dataset_name = "WKT";
  std::string output_csv;
  std::size_t limit = 1000000;
  std::uint64_t seed = 42;
  double piece_limit = 10000.0;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  bool append_csv = false;
};

struct LoadStats {
  std::size_t lines_seen = 0;
  std::size_t parse_errors = 0;
  long long load_ns = 0;
};

struct SizeRow {
  std::string index;
  std::string size_method;
  long long build_ns = 0;
  long long index_bytes = 0;
  std::size_t node_count = 0;
  std::size_t internal_nodes = 0;
  std::size_t leaf_nodes = 0;
  std::size_t entry_count = 0;
  std::size_t depth = 0;
  long long glin_model_bytes = 0;
  long long glin_data_bytes = 0;
  long long glin_piecewise_bytes = 0;
  std::size_t pieces = 0;
  std::size_t boost_internal_node_bytes = 0;
  std::size_t boost_leaf_node_bytes = 0;
  std::size_t quad_node_bytes = 0;
  std::size_t quad_item_ref_bytes = 0;
  std::string note;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file /path/to/data.wkt [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME   Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N             Number of valid geometries to load (default: 1000000)\n"
      << "  --seed N              Recorded in CSV for comparability (default: 42)\n"
      << "  --piece_limit N       Recorded only; Fig.8 excludes piecewise bytes (default: 10000)\n"
      << "  --cell_xmin X         Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y         Z-order latitude origin (default: -90)\n"
      << "  --cell_size S         Z-order cell size for x/y (default: 5e-7)\n"
      << "  --output_csv PATH     Write summary CSV rows\n"
      << "  --append_csv          Append rows to output CSV\n";
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
    } else if (key == "--output_csv") {
      options.output_csv = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
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
  return "";
}

std::vector<GeometryPtr> load_wkt_csv(const Options& options,
                                      geos::io::WKTReader& reader,
                                      LoadStats& stats) {
  std::ifstream input(options.data_file);
  if (!input) {
    throw std::runtime_error("Cannot open data file: " + options.data_file);
  }

  auto start = std::chrono::high_resolution_clock::now();
  std::vector<GeometryPtr> geometries;
  geometries.reserve(options.limit);
  std::string line;
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
    } catch (const geos::util::GEOSException&) {
      ++stats.parse_errors;
    } catch (const std::exception&) {
      ++stats.parse_errors;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  stats.load_ns = ns_count(end - start);
  return geometries;
}

std::vector<Geometry*> raw_ptrs(const std::vector<GeometryPtr>& geometries) {
  std::vector<Geometry*> raw;
  raw.reserve(geometries.size());
  for (const auto& geometry : geometries) {
    raw.push_back(geometry.get());
  }
  return raw;
}

Box box_from_envelope(const geos::geom::Envelope* envelope) {
  return Box(Point(envelope->getMinX(), envelope->getMinY()),
             Point(envelope->getMaxX(), envelope->getMaxY()));
}

std::vector<RTreeValue> make_rtree_values(
    const std::vector<GeometryPtr>& geometries) {
  std::vector<RTreeValue> values;
  values.reserve(geometries.size());
  for (std::size_t i = 0; i < geometries.size(); ++i) {
    values.emplace_back(box_from_envelope(geometries[i]->getEnvelopeInternal()),
                        i);
  }
  return values;
}

SizeRow measure_glin(const Options& options,
                     const std::vector<GeometryPtr>& geometries) {
  SizeRow row;
  row.index = "GLIN";
  row.size_method = "alex_model_size_excludes_piecewise";
  row.entry_count = geometries.size();
  row.note = "Fig.8 uses GLIN core index size only; piecewise function bytes are excluded.";

  std::vector<Geometry*> raw = raw_ptrs(geometries);
  std::vector<std::tuple<double, double, double, double>> pieces;
  alex::Glin<double, Geometry*> index;

  auto start = std::chrono::high_resolution_clock::now();
  index.glin_bulk_load(raw, options.piece_limit, "z", options.cell_xmin,
                       options.cell_ymin, options.cell_size, options.cell_size,
                       pieces);
  auto end = std::chrono::high_resolution_clock::now();

  row.build_ns = ns_count(end - start);
  row.glin_model_bytes = index.model_size();
  row.glin_data_bytes = index.data_size();
  row.glin_piecewise_bytes =
      static_cast<long long>(pieces.size() *
                             sizeof(std::tuple<double, double, double, double>));
  row.pieces = pieces.size();
  row.index_bytes = row.glin_model_bytes;
  row.node_count = static_cast<std::size_t>(index.num_nodes());
  row.leaf_nodes = static_cast<std::size_t>(index.num_leaves());
  row.internal_nodes = row.node_count - row.leaf_nodes;
  return row;
}

template <typename Value, typename Options, typename BoxType,
          typename Allocators>
struct BoostNodeCounter
    : public boost::geometry::index::detail::rtree::visitor<
          Value, typename Options::parameters_type, BoxType, Allocators,
          typename Options::node_tag, true>::type {
  typedef boost::geometry::index::detail::rtree::visitor<
      Value, typename Options::parameters_type, BoxType, Allocators,
      typename Options::node_tag, true>
      VisitorBase;
  typedef typename boost::geometry::index::detail::rtree::internal_node<
      Value, typename Options::parameters_type, BoxType, Allocators,
      typename Options::node_tag>::type internal_node;
  typedef typename boost::geometry::index::detail::rtree::leaf<
      Value, typename Options::parameters_type, BoxType, Allocators,
      typename Options::node_tag>::type leaf;

  std::size_t internal_size = 0;
  std::size_t leaf_size = 0;

  void operator()(internal_node const& node) {
    typedef typename boost::geometry::index::detail::rtree::elements_type<
        internal_node>::type elements_type;
    elements_type const& elements =
        boost::geometry::index::detail::rtree::elements(node);

    ++internal_size;
    for (typename elements_type::const_iterator it = elements.begin();
         it != elements.end(); ++it) {
      boost::geometry::index::detail::rtree::apply_visitor(*this, *it->second);
    }
  }

  void operator()(leaf const&) { ++leaf_size; }
};

template <typename RTreeType>
std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>
boost_node_stats(const RTreeType& tree) {
  typedef typename RTreeType::value_type Value;
  typedef typename RTreeType::options_type Options;
  typedef typename RTreeType::box_type BoxType;
  typedef typename RTreeType::allocators_type Allocators;
  typedef BoostNodeCounter<Value, Options, BoxType, Allocators> Counter;

  boost::geometry::index::detail::rtree::utilities::view<RTreeType> view(tree);
  Counter counter;
  view.apply_visitor(counter);
  return std::make_tuple(counter.internal_size, counter.leaf_size,
                         sizeof(typename Counter::internal_node),
                         sizeof(typename Counter::leaf));
}

SizeRow measure_boost_rtree(const std::vector<GeometryPtr>& geometries) {
  SizeRow row;
  row.index = "Boost_Rtree";
  row.size_method = "boost_node_count_times_static_node_size";
  row.entry_count = geometries.size();
  row.note = "Estimated from Boost R-tree internal/leaf node counts and static node sizeof().";

  std::vector<RTreeValue> values = make_rtree_values(geometries);
  auto start = std::chrono::high_resolution_clock::now();
  RTree rtree(values.begin(), values.end());
  auto end = std::chrono::high_resolution_clock::now();

  std::size_t internal_node_size = 0;
  std::size_t leaf_node_size = 0;
  std::tie(row.internal_nodes, row.leaf_nodes, internal_node_size,
           leaf_node_size) = boost_node_stats(rtree);
  row.build_ns = ns_count(end - start);
  row.node_count = row.internal_nodes + row.leaf_nodes;
  row.boost_internal_node_bytes = internal_node_size;
  row.boost_leaf_node_bytes = leaf_node_size;
  row.index_bytes =
      static_cast<long long>(row.internal_nodes * internal_node_size +
                             row.leaf_nodes * leaf_node_size);
  return row;
}

SizeRow measure_geos_quadtree(const std::vector<GeometryPtr>& geometries) {
  SizeRow row;
  row.index = "GEOS_Quadtree";
  row.size_method = "geos_private_node_count_estimate";
  row.entry_count = geometries.size();
  row.note = "Estimated from GEOS quadtree node count plus per-item envelope/pointer references.";

  Quadtree quadtree;
  auto start = std::chrono::high_resolution_clock::now();
  for (const auto& geometry : geometries) {
    quadtree.insert(geometry->getEnvelopeInternal(), geometry.get());
  }
  auto end = std::chrono::high_resolution_clock::now();

  row.build_ns = ns_count(end - start);
  row.node_count = quadtree.root.getNodeCount();
  row.entry_count = quadtree.size();
  row.depth = static_cast<std::size_t>(quadtree.depth());
  row.quad_node_bytes = sizeof(geos::index::quadtree::NodeBase);
  row.quad_item_ref_bytes = sizeof(void*) + sizeof(geos::geom::Envelope);
  row.index_bytes =
      static_cast<long long>(row.node_count * row.quad_node_bytes +
                             row.entry_count * row.quad_item_ref_bytes);
  return row;
}

void write_csv(const Options& options, const LoadStats& stats,
               std::size_t loaded_count, const std::vector<SizeRow>& rows) {
  if (options.output_csv.empty()) {
    return;
  }

  bool write_header = true;
  if (options.append_csv) {
    std::ifstream existing(options.output_csv);
    write_header = !existing.good() || existing.peek() == std::ifstream::traits_type::eof();
  }

  std::ofstream output(options.output_csv,
                       options.append_csv ? std::ios::app : std::ios::out);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + options.output_csv);
  }

  if (write_header) {
    output << "dataset,index,loaded_count,limit,piece_limit,cell_xmin,cell_ymin,"
              "cell_size,seed,lines_seen,parse_errors,load_ns,build_ns,"
              "index_bytes,index_mib,size_method,node_count,internal_nodes,"
              "leaf_nodes,entry_count,depth,glin_model_bytes,glin_data_bytes,"
              "glin_piecewise_bytes,pieces,boost_internal_node_bytes,"
              "boost_leaf_node_bytes,quad_node_bytes,quad_item_ref_bytes,note\n";
  }

  output << std::fixed << std::setprecision(9);
  for (const auto& row : rows) {
    output << options.dataset_name << "," << row.index << "," << loaded_count
           << "," << options.limit << "," << options.piece_limit << ","
           << options.cell_xmin << "," << options.cell_ymin << ","
           << options.cell_size << "," << options.seed << ","
           << stats.lines_seen << "," << stats.parse_errors << ","
           << stats.load_ns << "," << row.build_ns << "," << row.index_bytes
           << "," << (static_cast<double>(row.index_bytes) / 1048576.0)
           << "," << row.size_method << "," << row.node_count << ","
           << row.internal_nodes << "," << row.leaf_nodes << ","
           << row.entry_count << "," << row.depth << ","
           << row.glin_model_bytes << "," << row.glin_data_bytes << ","
           << row.glin_piecewise_bytes << "," << row.pieces << ","
           << row.boost_internal_node_bytes << ","
           << row.boost_leaf_node_bytes << "," << row.quad_node_bytes << ","
           << row.quad_item_ref_bytes << ",\"" << row.note << "\"\n";
  }
}

void print_rows(const Options& options, const LoadStats& stats,
                std::size_t loaded_count, const std::vector<SizeRow>& rows) {
  std::cout << std::fixed << std::setprecision(3);
  for (const auto& row : rows) {
    std::cout << "dataset=" << options.dataset_name << " index=" << row.index
              << " loaded=" << loaded_count
              << " lines_seen=" << stats.lines_seen
              << " parse_errors=" << stats.parse_errors
              << " build_ms=" << row.build_ns / 1e6
              << " index_mib="
              << static_cast<double>(row.index_bytes) / 1048576.0
              << " nodes=" << row.node_count
              << " method=" << row.size_method << "\n";
  }
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

    std::vector<SizeRow> rows;
    rows.reserve(3);
    rows.push_back(measure_glin(options, geometries));
    rows.push_back(measure_boost_rtree(geometries));
    rows.push_back(measure_geos_quadtree(geometries));

    write_csv(options, stats, geometries.size(), rows);
    print_rows(options, stats, geometries.size(), rows);
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }
  return 0;
}
