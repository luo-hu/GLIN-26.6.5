// 从“一行一个 WKT”的 CSV 数据里生成固定查询窗口。
//
// 这个程序是论文复现实验的第二步：先生成一份 query 文件，后续所有索引都读同一份
// query 文件来测试。这样 GLIN、R-tree、QuadTree 的结果才有可比性。
//
// 默认生成近似论文的 geometry-distance KNN selectivity workload：
// 1. 随机选一个 geometry 作为中心。
// 2. 用 Boost R-tree 的 MBR nearest 先取一批候选。
// 3. 对候选调用 GEOS Geometry::distance() 精排，取 K 个近邻。
// 4. 把这 K 个近邻 geometry 的 MBR 合并成查询窗口。
//
// 注意：论文使用 JTS STR-Tree 做 KNN。这里为了保持 C++ 工具链简单，用 Boost R-tree
// 做候选召回、GEOS geometry distance 做精排；这比“中心点 KNN”更接近论文 workload，
// 但仍不是 JTS STR-Tree 的完全复刻。

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
using Point = bg::model::point<double, 2, bg::cs::cartesian>;
using Box = bg::model::box<Point>;
using CenterRTreeValue = std::pair<Point, std::size_t>;
using CenterRTree = bgi::rtree<CenterRTreeValue, bgi::linear<16>>;
using BoxRTreeValue = std::pair<Box, std::size_t>;
using BoxRTree = bgi::rtree<BoxRTreeValue, bgi::linear<16>>;

struct Options {
  std::string data_file;       // 输入 WKT CSV，例如 /mnt/hgfs/AREAWATER.csv。
  std::string output_file;     // 输出 query CSV。
  std::string output_prefix;   // 多个 selectivity 时使用的输出前缀。
  std::string mode = "geom_knn";  // geom_knn：更接近论文；knn：中心点近似；mbr：旧行为。
  std::size_t limit = 10000;   // 只从前 N 条合法 geometry 中抽 query。
  std::size_t query_count = 100;  // 生成多少个查询窗口。
  std::uint64_t seed = 42;     // 固定随机种子，保证可复现。
  std::vector<double> selectivities = {0.01, 0.001, 0.0001, 0.00001};
  double candidate_multiplier = 8.0;  // geom_knn 候选池倍数：pool ~= K * multiplier。
  std::size_t min_candidate_pool = 1000;  // 小 K 时至少精排这么多候选。
};

struct GeometryDistance {
  std::size_t id = 0;
  double distance = 0.0;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --data_file /path/to/AREAWATER.csv --output_file queries/aw_queries.csv [options]\n"
      << "Options:\n"
      << "  --limit N             Number of valid geometries to load (default: 10000)\n"
      << "  --query_count N       Number of query windows to generate (default: 100)\n"
      << "  --mode geom_knn|knn|mbr  geom_knn uses GEOS distance; knn uses center points; mbr keeps old behavior (default: geom_knn)\n"
      << "  --selectivities LIST  Comma list, e.g. 1%,0.1%,0.01%,0.001% (default: paper list)\n"
      << "  --output_prefix PATH  Prefix for multiple selectivity files, e.g. queries/aw1m_knn\n"
      << "  --candidate_multiplier X  geom_knn candidate pool multiplier (default: 8)\n"
      << "  --min_candidate_pool N    geom_knn minimum candidate pool size (default: 1000)\n"
      << "  --seed N              Random seed (default: 42)\n";
}

std::string trim(std::string value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<double> parse_selectivities(const std::string& text) {
  std::vector<double> selectivities;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (item.empty()) {
      continue;
    }

    bool percent = false;
    if (!item.empty() && item.back() == '%') {
      percent = true;
      item.pop_back();
    }

    double value = std::stod(item);
    if (percent) {
      value /= 100.0;
    }
    if (value <= 0.0 || value > 1.0) {
      throw std::runtime_error("Selectivity must be in (0, 1], got: " + item);
    }
    selectivities.push_back(value);
  }

  if (selectivities.empty()) {
    throw std::runtime_error("--selectivities produced an empty list");
  }
  return selectivities;
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
    } else if (key == "--output_file") {
      options.output_file = require_value(key);
    } else if (key == "--output_prefix") {
      options.output_prefix = require_value(key);
    } else if (key == "--mode") {
      options.mode = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--query_count") {
      options.query_count = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--selectivities") {
      options.selectivities = parse_selectivities(require_value(key));
    } else if (key == "--candidate_multiplier") {
      options.candidate_multiplier = std::stod(require_value(key));
    } else if (key == "--min_candidate_pool") {
      options.min_candidate_pool =
          static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
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
  if (options.output_file.empty() && options.output_prefix.empty()) {
    throw std::runtime_error("--output_file or --output_prefix is required");
  }
  if (options.mode != "geom_knn" && options.mode != "knn" &&
      options.mode != "mbr") {
    throw std::runtime_error("--mode must be geom_knn, knn, or mbr");
  }
  if (options.mode == "mbr" && !options.output_prefix.empty() &&
      options.output_file.empty()) {
    throw std::runtime_error("--mode mbr needs --output_file");
  }
  if ((options.mode == "geom_knn" || options.mode == "knn") &&
      options.selectivities.size() > 1 &&
      !options.output_file.empty() && options.output_prefix.empty()) {
    throw std::runtime_error(
        "Multiple selectivities need --output_prefix, not only --output_file");
  }
  if (options.candidate_multiplier < 1.0) {
    throw std::runtime_error("--candidate_multiplier must be >= 1");
  }
  if (options.limit == 0) {
    throw std::runtime_error("--limit must be greater than 0");
  }
  if (options.query_count == 0) {
    throw std::runtime_error("--query_count must be greater than 0");
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

// 按括号配平截取 WKT，可处理 WKT 后面拼接额外属性字段的脏行。
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

// 不能按逗号拆 AREAWATER.csv，因为 WKT 内部自己就有大量逗号。
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
      "LINESTRING", "POINT"};
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
                                      std::size_t& lines_seen,
                                      std::size_t& parse_errors) {
  std::ifstream input(options.data_file);
  if (!input) {
    throw std::runtime_error("Cannot open data file: " + options.data_file);
  }

  std::vector<GeometryPtr> geometries;
  geometries.reserve(options.limit);
  std::string line;
  while (geometries.size() < options.limit && std::getline(input, line)) {
    ++lines_seen;
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
      ++parse_errors;
    } catch (const std::exception&) {
      ++parse_errors;
    }
  }
  return geometries;
}

std::string selectivity_tag(double selectivity) {
  if (selectivity >= 0.01) {
    return "1pct";
  }
  if (selectivity >= 0.001) {
    return "0p1pct";
  }
  if (selectivity >= 0.0001) {
    return "0p01pct";
  }
  if (selectivity >= 0.00001) {
    return "0p001pct";
  }

  std::ostringstream out;
  out << std::setprecision(8) << selectivity;
  std::string tag = out.str();
  for (char& ch : tag) {
    if (ch == '.') {
      ch = 'p';
    } else if (ch == '-') {
      ch = 'm';
    }
  }
  return tag;
}

std::string output_path_for_selectivity(const Options& options,
                                        double selectivity) {
  if (!options.output_prefix.empty()) {
    return options.output_prefix + "_" + selectivity_tag(selectivity) + ".csv";
  }
  return options.output_file;
}

double envelope_center_x(const geos::geom::Envelope* envelope) {
  return (envelope->getMinX() + envelope->getMaxX()) / 2.0;
}

double envelope_center_y(const geos::geom::Envelope* envelope) {
  return (envelope->getMinY() + envelope->getMaxY()) / 2.0;
}

Box box_from_envelope(const geos::geom::Envelope* envelope) {
  return Box(Point(envelope->getMinX(), envelope->getMinY()),
             Point(envelope->getMaxX(), envelope->getMaxY()));
}

CenterRTree build_center_rtree(const std::vector<GeometryPtr>& geometries) {
  std::vector<CenterRTreeValue> values;
  values.reserve(geometries.size());
  for (std::size_t i = 0; i < geometries.size(); ++i) {
    const geos::geom::Envelope* envelope = geometries[i]->getEnvelopeInternal();
    values.emplace_back(
        Point(envelope_center_x(envelope), envelope_center_y(envelope)), i);
  }
  return CenterRTree(values.begin(), values.end());
}

BoxRTree build_box_rtree(const std::vector<GeometryPtr>& geometries) {
  std::vector<BoxRTreeValue> values;
  values.reserve(geometries.size());
  for (std::size_t i = 0; i < geometries.size(); ++i) {
    values.emplace_back(box_from_envelope(geometries[i]->getEnvelopeInternal()), i);
  }
  return BoxRTree(values.begin(), values.end());
}

std::size_t k_for_selectivity(double selectivity, std::size_t loaded_count) {
  std::size_t k =
      static_cast<std::size_t>(std::ceil(selectivity * loaded_count));
  if (k < 1) {
    k = 1;
  }
  if (k > loaded_count) {
    k = loaded_count;
  }
  return k;
}

void expand_to_include(geos::geom::Envelope& query_envelope,
                       const geos::geom::Envelope* item_envelope) {
  query_envelope.expandToInclude(item_envelope->getMinX(), item_envelope->getMinY());
  query_envelope.expandToInclude(item_envelope->getMaxX(), item_envelope->getMaxY());
}

void write_query_row(std::ofstream& output, std::size_t query_id,
                     const geos::geom::Envelope& envelope,
                     std::size_t source_id, double selectivity,
                     std::size_t k, std::size_t candidate_pool,
                     const std::string& mode) {
  output << query_id << "," << envelope.getMinX() << "," << envelope.getMinY()
         << "," << envelope.getMaxX() << "," << envelope.getMaxY() << ","
         << source_id << "," << std::setprecision(17) << selectivity << "," << k
         << "," << candidate_pool << "," << mode
         << "\n";
}

void write_mbr_queries(const Options& options,
                       const std::vector<GeometryPtr>& geometries) {
  std::ofstream output(options.output_file);
  if (!output) {
    throw std::runtime_error("Cannot open output file: " + options.output_file);
  }

  std::mt19937_64 rng(options.seed);
  std::uniform_int_distribution<std::size_t> distribution(0, geometries.size() - 1);

  output << "query_id,xmin,ymin,xmax,ymax,source_geometry_id,selectivity,k,"
            "candidate_pool,mode\n";
  output << std::setprecision(17);
  for (std::size_t query_id = 0; query_id < options.query_count; ++query_id) {
    std::size_t source_id = distribution(rng);
    const geos::geom::Envelope* envelope = geometries[source_id]->getEnvelopeInternal();
    geos::geom::Envelope query_envelope(*envelope);
    write_query_row(output, query_id, query_envelope, source_id, 0.0, 1, 1,
                    options.mode);
  }
}

void write_knn_queries_for_selectivity(const Options& options,
                                       const std::vector<GeometryPtr>& geometries,
                                       const CenterRTree& rtree,
                                       double selectivity) {
  const std::string output_file = output_path_for_selectivity(options, selectivity);
  std::ofstream output(output_file);
  if (!output) {
    throw std::runtime_error("Cannot open output file: " + output_file);
  }

  const std::size_t k = k_for_selectivity(selectivity, geometries.size());
  std::mt19937_64 rng(options.seed);
  std::uniform_int_distribution<std::size_t> distribution(0, geometries.size() - 1);

  output << "query_id,xmin,ymin,xmax,ymax,source_geometry_id,selectivity,k,"
            "candidate_pool,mode\n";
  output << std::setprecision(17);
  for (std::size_t query_id = 0; query_id < options.query_count; ++query_id) {
    const std::size_t source_id = distribution(rng);
    const geos::geom::Envelope* source_envelope =
        geometries[source_id]->getEnvelopeInternal();
    Point center(envelope_center_x(source_envelope),
                 envelope_center_y(source_envelope));

    std::vector<CenterRTreeValue> neighbors;
    neighbors.reserve(k);
    rtree.query(bgi::nearest(center, static_cast<unsigned>(k)),
                std::back_inserter(neighbors));

    geos::geom::Envelope query_envelope(*source_envelope);
    for (const auto& neighbor : neighbors) {
      expand_to_include(query_envelope,
                        geometries[neighbor.second]->getEnvelopeInternal());
    }

    write_query_row(output, query_id, query_envelope, source_id, selectivity, k, k,
                    options.mode);
  }

  std::cout << "query_file=" << output_file
            << " selectivity=" << std::setprecision(17) << selectivity
            << " k=" << k
            << " query_count=" << options.query_count
            << "\n";
}

void write_knn_queries(const Options& options,
                       const std::vector<GeometryPtr>& geometries) {
  CenterRTree rtree = build_center_rtree(geometries);
  for (double selectivity : options.selectivities) {
    write_knn_queries_for_selectivity(options, geometries, rtree, selectivity);
  }
}

std::size_t candidate_pool_for_k(const Options& options, std::size_t k,
                                 std::size_t loaded_count) {
  std::size_t pool = static_cast<std::size_t>(
      std::ceil(static_cast<double>(k) * options.candidate_multiplier));
  if (pool < options.min_candidate_pool) {
    pool = options.min_candidate_pool;
  }
  if (pool < k) {
    pool = k;
  }
  if (pool > loaded_count) {
    pool = loaded_count;
  }
  return pool;
}

void sort_geometry_distances(std::vector<GeometryDistance>& distances) {
  std::sort(distances.begin(), distances.end(),
            [](const GeometryDistance& left, const GeometryDistance& right) {
              if (left.distance == right.distance) {
                return left.id < right.id;
              }
              return left.distance < right.distance;
            });
}

void write_geometry_knn_queries_for_selectivity(
    const Options& options, const std::vector<GeometryPtr>& geometries,
    const BoxRTree& rtree, double selectivity) {
  const std::string output_file = output_path_for_selectivity(options, selectivity);
  std::ofstream output(output_file);
  if (!output) {
    throw std::runtime_error("Cannot open output file: " + output_file);
  }

  const std::size_t k = k_for_selectivity(selectivity, geometries.size());
  const std::size_t candidate_pool =
      candidate_pool_for_k(options, k, geometries.size());
  std::mt19937_64 rng(options.seed);
  std::uniform_int_distribution<std::size_t> distribution(0, geometries.size() - 1);

  output << "query_id,xmin,ymin,xmax,ymax,source_geometry_id,selectivity,k,"
            "candidate_pool,mode\n";
  output << std::setprecision(17);
  for (std::size_t query_id = 0; query_id < options.query_count; ++query_id) {
    const std::size_t source_id = distribution(rng);
    Geometry* source_geometry = geometries[source_id].get();
    const geos::geom::Envelope* source_envelope =
        source_geometry->getEnvelopeInternal();
    const Box source_box = box_from_envelope(source_envelope);

    std::vector<BoxRTreeValue> box_neighbors;
    box_neighbors.reserve(candidate_pool);
    rtree.query(bgi::nearest(source_box, static_cast<unsigned>(candidate_pool)),
                std::back_inserter(box_neighbors));

    std::vector<GeometryDistance> distances;
    distances.reserve(box_neighbors.size());
    for (const auto& neighbor : box_neighbors) {
      GeometryDistance item;
      item.id = neighbor.second;
      item.distance = source_geometry->distance(geometries[item.id].get());
      distances.push_back(item);
    }
    sort_geometry_distances(distances);

    geos::geom::Envelope query_envelope(*source_envelope);
    const std::size_t neighbor_count = std::min(k, distances.size());
    for (std::size_t i = 0; i < neighbor_count; ++i) {
      expand_to_include(query_envelope,
                        geometries[distances[i].id]->getEnvelopeInternal());
    }

    write_query_row(output, query_id, query_envelope, source_id, selectivity, k,
                    candidate_pool, options.mode);
  }

  std::cout << "query_file=" << output_file
            << " selectivity=" << std::setprecision(17) << selectivity
            << " k=" << k
            << " candidate_pool=" << candidate_pool
            << " query_count=" << options.query_count
            << "\n";
}

void write_geometry_knn_queries(const Options& options,
                                const std::vector<GeometryPtr>& geometries) {
  BoxRTree rtree = build_box_rtree(geometries);
  for (double selectivity : options.selectivities) {
    write_geometry_knn_queries_for_selectivity(options, geometries, rtree,
                                               selectivity);
  }
}

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    std::size_t lines_seen = 0;
    std::size_t parse_errors = 0;
    auto load_start = std::chrono::high_resolution_clock::now();
    std::vector<GeometryPtr> geometries =
        load_wkt_csv(options, reader, lines_seen, parse_errors);
    auto load_end = std::chrono::high_resolution_clock::now();

    if (geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }

    auto query_start = std::chrono::high_resolution_clock::now();
    if (options.mode == "geom_knn") {
      write_geometry_knn_queries(options, geometries);
    } else if (options.mode == "knn") {
      write_knn_queries(options, geometries);
    } else {
      write_mbr_queries(options, geometries);
    }
    auto query_end = std::chrono::high_resolution_clock::now();

    std::cout << "mode=" << options.mode
              << " lines_seen=" << lines_seen
              << " loaded=" << geometries.size()
              << " parse_errors=" << parse_errors
              << " query_count=" << options.query_count
              << " seed=" << options.seed
              << " load_ms=" << ns_count(load_end - load_start) / 1e6
              << " generate_ms=" << ns_count(query_end - query_start) / 1e6
              << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
