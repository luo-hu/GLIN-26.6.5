// 一个最小版 GLIN 实验程序：读取“一行一个 WKT”的 CSV 文件，建立 GLIN 索引并跑查询。
//
// 这个程序不是论文完整复现实验，只是第一步：
// 1. 确认 AREAWATER.csv 能被正确读入。
// 2. 确认 GLIN / GLIN-piecewise 能在真实 WKT 数据上建索引、查询、输出结果。
// 3. 输出 CSV，方便后面接论文式 query workload 和画图脚本。

#include "../../glin/glin.h"

#include <geos/geom/Coordinate.h>
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/LinearRing.h>
#include <geos/geom/Polygon.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;

struct Options {
  std::string data_file;       // 输入数据文件，例如 /mnt/hgfs/AREAWATER.csv。
  std::string dataset_name = "WKT";  // 输出 CSV 里的数据集名称，例如 AREAWATER 或 PARKS。
  std::string query_file;      // 固定查询窗口文件；为空时使用随机抽样 smoke test。
  std::string output_csv;      // 每条查询的结果输出文件；为空时只在终端打印汇总。
  std::size_t limit = 10000;   // 最多读取多少条合法 WKT。用于先小规模试跑，避免一上来读 229 万行。
  std::size_t query_count = 20;  // 未指定 query_file 时，随机抽多少个已加载 geometry 作为查询窗口。
  std::uint64_t seed = 42;     // 随机种子。固定它可以让每次抽到同一批 query。
  double piece_limit = 10000.0;  // 每个 piece 汇总多少条记录；只在 PIECE 版本中真正起作用。
  double cell_xmin = -180.0;   // 经度转 Z-order 整数坐标时的起点，按论文公式设置。
  double cell_ymin = -90.0;    // 纬度转 Z-order 整数坐标时的起点，按论文公式设置。
  double cell_size = 0.0000005;  // Z-order 网格大小，论文默认 5e-7。
};

struct QueryResult {
  std::size_t query_id = 0;  // 查询编号；随机模式下就是第几次查询，query_file 模式下来自文件。
  std::size_t source_geometry_id = 0;  // 生成该 query 的原始 geometry 下标。
  long long probe_ns = 0;   // 索引探测耗时：用 GLIN 快速找到候选范围。
  long long refine_ns = 0;  // 精确过滤耗时：用 GEOS contains/intersects 检查候选结果。
  int candidates = 0;       // refine 前候选对象数量，越大通常 refine 越慢。
  std::size_t answers = 0;  // refine 后真正满足关系的结果数量。
  double visited_leaf = 0.0;  // 查询访问过的 GLIN 叶节点数，来自 GLIN 迭代器统计。
  double loaded_leaf = 0.0;   // 查询实际加载/检查过的 GLIN 叶节点数。
};

struct QueryCase {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  Geometry* geometry = nullptr;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file /path/to/AREAWATER.csv [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME   Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N             Number of valid geometries to load (default: 10000)\n"
      << "  --query_count N       Number of sampled queries to run (default: 20)\n"
      << "  --query_file PATH     Fixed query CSV: query_id,xmin,ymin,xmax,ymax,source_geometry_id\n"
      << "  --seed N              Random seed for query sampling (default: 42)\n"
      << "  --piece_limit N       Records per piece for PIECE build (default: 10000)\n"
      << "  --cell_xmin X         Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y         Z-order latitude origin (default: -90)\n"
      << "  --cell_size S         Z-order cell size for x/y (default: 5e-7)\n"
      << "  --output_csv PATH     Write per-query CSV rows\n";
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
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--query_count") {
      options.query_count = static_cast<std::size_t>(std::stoull(require_value(key)));
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
  return options;
}

void strip_bom(std::string& line) {
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
}

// Windows 文件常见行尾是 \r\n；std::getline 去掉 \n 后，末尾可能还剩一个 \r。
void strip_cr(std::string& line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
}

// 从一行文本中截出括号平衡的 WKT。
// 这样可以处理第 56735 行那种情况：前半段是合法 WKT，后面还拼了地名/编号等脏字段。
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

// AREAWATER.csv 不是普通多列 CSV，不能按逗号切。
// 每行核心是一个 WKT，格式可能是：
//   "POLYGON ((...))"
// 或者：
//   "POLYGON ((...))"  其他属性字段
// 这里的策略是：先去 BOM 和 \r，再取外层引号里的内容；如果引号不干净，就按括号配平截 WKT。
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

// GLIN 的接口要 vector<Geometry*>；这里把 unique_ptr 管理的对象转成裸指针视图。
// 真正负责释放内存的仍然是 owned_geometries，不要 delete 这些裸指针。
std::vector<Geometry*> raw_ptrs(const std::vector<GeometryPtr>& geometries) {
  std::vector<Geometry*> raw;
  raw.reserve(geometries.size());
  for (const auto& geometry : geometries) {
    raw.push_back(geometry.get());
  }
  return raw;
}

// 逐行读取 WKT，并用 GEOS WKTReader 转成 Geometry 对象。
// parse_errors 只统计解析失败的行；本函数会跳过坏行，继续读到 limit 条合法 geometry。
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
  std::size_t first_error_line = 0;
  std::string first_error_message;

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
    } catch (const geos::util::GEOSException& ex) {
      ++parse_errors;
      if (first_error_line == 0) {
        first_error_line = lines_seen;
        first_error_message = ex.what();
      }
    } catch (const std::exception& ex) {
      ++parse_errors;
      if (first_error_line == 0) {
        first_error_line = lines_seen;
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

// 当前 smoke benchmark 没有生成论文里的 KNN query workload。
// 它只是随机抽 query_count 个已加载 geometry，把它们自己当作查询窗口。
// 这适合验证程序链路，不适合直接画论文图。
std::vector<std::size_t> sample_query_ids(std::size_t query_count,
                                          std::size_t num_geometries,
                                          std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> distribution(0, num_geometries - 1);
  std::vector<std::size_t> ids;
  ids.reserve(query_count);
  for (std::size_t i = 0; i < query_count; ++i) {
    ids.push_back(distribution(rng));
  }
  return ids;
}

// 用 xmin/ymin/xmax/ymax 构造一个矩形 polygon，作为 query window。
// GEOS 的 contains/intersects 都需要一个 Geometry，这里不能只传 Envelope。
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
    double xmin = std::stod(fields[1]);
    double ymin = std::stod(fields[2]);
    double xmax = std::stod(fields[3]);
    double ymax = std::stod(fields[4]);
    query.source_geometry_id = static_cast<std::size_t>(std::stoull(fields[5]));

    owned_queries.push_back(make_query_box(factory, xmin, ymin, xmax, ymax));
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
  std::vector<std::size_t> query_ids =
      sample_query_ids(query_count, geometries.size(), seed);
  std::vector<QueryCase> queries;
  queries.reserve(query_ids.size());
  for (std::size_t i = 0; i < query_ids.size(); ++i) {
    QueryCase query;
    query.query_id = i;
    query.source_geometry_id = query_ids[i];
    query.geometry = geometries[query_ids[i]];
    queries.push_back(query);
  }
  return queries;
}

// 把每一次查询的详细结果写成 CSV。后续 Python 画图或聚合平均值就读这个文件。
void write_csv(const std::string& path, const Options& options,
               std::size_t loaded_count, long long build_ns,
               const std::vector<QueryResult>& results) {
  if (path.empty()) {
    return;
  }

  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + path);
  }

  output << "dataset,index,relationship,loaded_count,piece_limit,cell_xmin,"
            "cell_ymin,cell_size,seed,build_ns,query_id,source_geometry_id,probe_ns,refine_ns,"
            "total_ns,candidates,answers,visited_leaf,loaded_leaf\n";

#ifdef PIECE
  // 编译 bench_glin_wkt_piece 时定义 PIECE：
  // GLIN 会构建 piecewise function，refine 阶段检查 intersects。
  const char* index_name = "GLIN_PIECEWISE";
  const char* relationship = "intersects";
#else
  // 编译 bench_glin_wkt 时没有 PIECE：
  // GLIN 不做 query augmentation，refine 阶段检查 contains。
  const char* index_name = "GLIN";
  const char* relationship = "contains";
#endif

  for (const auto& result : results) {
    output << options.dataset_name << "," << index_name << "," << relationship << ","
           << loaded_count << "," << options.piece_limit << ","
           << options.cell_xmin << "," << options.cell_ymin << ","
           << options.cell_size << "," << options.seed << "," << build_ns
           << "," << result.query_id << "," << result.source_geometry_id << ","
           << result.probe_ns << ","
           << result.refine_ns << "," << (result.probe_ns + result.refine_ns)
           << "," << result.candidates << "," << result.answers << ","
           << result.visited_leaf << "," << result.loaded_leaf << "\n";
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

    // GEOS 的 GeometryFactory 和 WKTReader 负责把 WKT 字符串解析成几何对象。
    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    // 1. 读取数据文件。这个时间包含磁盘/共享目录 I/O 和 GEOS WKT 解析时间。
    std::size_t lines_seen = 0;
    std::size_t parse_errors = 0;
    auto load_start = std::chrono::high_resolution_clock::now();
    std::vector<GeometryPtr> owned_geometries =
        load_wkt_csv(options, reader, lines_seen, parse_errors);
    auto load_end = std::chrono::high_resolution_clock::now();

    if (owned_geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }

    std::vector<Geometry*> geometries = raw_ptrs(owned_geometries);
    std::vector<std::tuple<double, double, double, double>> pieces;

    // 2. 建 GLIN 索引。这里才是 index build time，不包含上面的文件读取时间。
    alex::Glin<double, Geometry*> index;
    auto build_start = std::chrono::high_resolution_clock::now();
    index.glin_bulk_load(geometries, options.piece_limit, "z",
                         options.cell_xmin, options.cell_ymin,
                         options.cell_size, options.cell_size, pieces);
    auto build_end = std::chrono::high_resolution_clock::now();
    long long build_ns = ns_count(build_end - build_start);

    // 3. 准备查询窗口：
    //    - 如果传了 --query_file，就读取固定 query 文件。
    //    - 如果没传，就临时随机抽 query_count 个 geometry 做 smoke test。
    std::vector<GeometryPtr> owned_queries;
    std::vector<QueryCase> queries =
        options.query_file.empty()
            ? make_random_queries(geometries, options.query_count, options.seed)
            : load_query_file(options.query_file, *factory, owned_queries);

    std::vector<QueryResult> results;
    results.reserve(queries.size());

    for (const auto& query : queries) {
      int count_filter = 0;
      std::vector<Geometry*> find_result;

      // glin_find 内部会先 index_probe，再 refine。
      // 无 PIECE 版本 refine 用 contains；PIECE 版本 refine 用 intersects。
      index.glin_find(query.geometry, "z", options.cell_xmin, options.cell_ymin,
                      options.cell_size, options.cell_size, pieces, find_result,
                      count_filter);

      QueryResult result;
      result.query_id = query.query_id;
      result.source_geometry_id = query.source_geometry_id;
      result.probe_ns = index.index_probe_duration.count();
      result.refine_ns = index.index_refine_duration.count();
      result.candidates = count_filter;
      result.answers = find_result.size();
      result.visited_leaf = index.avg_num_visited_leaf;
      result.loaded_leaf = index.avg_num_loaded_leaf;
      results.push_back(result);
    }

    long long total_probe_ns = 0;
    long long total_refine_ns = 0;
    long long total_candidates = 0;
    long long total_answers = 0;
    for (const auto& result : results) {
      total_probe_ns += result.probe_ns;
      total_refine_ns += result.refine_ns;
      total_candidates += result.candidates;
      total_answers += static_cast<long long>(result.answers);
    }

    write_csv(options.output_csv, options, geometries.size(), build_ns, results);

#ifdef PIECE
    // 终端汇总输出，方便快速看一眼本次运行是否正常。
    const char* index_name = "GLIN_PIECEWISE";
    const char* relationship = "intersects";
#else
    const char* index_name = "GLIN";
    const char* relationship = "contains";
#endif

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "dataset=" << options.dataset_name
              << " index=" << index_name
              << " relationship=" << relationship
              << " lines_seen=" << lines_seen
              << " loaded=" << geometries.size()
              << " parse_errors=" << parse_errors
              << " pieces=" << pieces.size()
              << " queries=" << results.size()
              << " load_ms=" << ns_count(load_end - load_start) / 1e6
              << " build_ms=" << build_ns / 1e6
              << " avg_probe_ns=" << total_probe_ns / results.size()
              << " avg_refine_ns=" << total_refine_ns / results.size()
              << " avg_total_ns="
              << (total_probe_ns + total_refine_ns) / results.size()
              << " avg_candidates="
              << static_cast<double>(total_candidates) / results.size()
              << " avg_answers="
              << static_cast<double>(total_answers) / results.size()
              << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
