// 从“一行一个 WKT”的 CSV 数据里生成固定查询窗口。
//
// 这个程序是论文复现实验的第二步：先生成一份 query 文件，后续所有索引都读同一份
// query 文件来测试。这样 GLIN、R-tree、QuadTree 的结果才有可比性。
//
// 注意：这里生成的是简化版 query workload，不是论文里的 JTS KNN selectivity workload。
// 当前做法是随机抽一些 geometry，然后把它们的 MBR 作为查询窗口。

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;

struct Options {
  std::string data_file;       // 输入 WKT CSV，例如 /mnt/hgfs/AREAWATER.csv。
  std::string output_file;     // 输出 query CSV。
  std::size_t limit = 10000;   // 只从前 N 条合法 geometry 中抽 query。
  std::size_t query_count = 100;  // 生成多少个查询窗口。
  std::uint64_t seed = 42;     // 固定随机种子，保证可复现。
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --data_file /path/to/AREAWATER.csv --output_file queries/aw_queries.csv [options]\n"
      << "Options:\n"
      << "  --limit N             Number of valid geometries to load (default: 10000)\n"
      << "  --query_count N       Number of query windows to generate (default: 100)\n"
      << "  --seed N              Random seed (default: 42)\n";
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
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--query_count") {
      options.query_count = static_cast<std::size_t>(std::stoull(require_value(key)));
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
  if (options.output_file.empty()) {
    throw std::runtime_error("--output_file is required");
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
      "MULTIPOLYGON", "POLYGON", "MULTILINESTRING", "LINESTRING", "POINT"};
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

void write_queries(const Options& options, const std::vector<GeometryPtr>& geometries) {
  std::ofstream output(options.output_file);
  if (!output) {
    throw std::runtime_error("Cannot open output file: " + options.output_file);
  }

  std::mt19937_64 rng(options.seed);
  std::uniform_int_distribution<std::size_t> distribution(0, geometries.size() - 1);

  output << "query_id,xmin,ymin,xmax,ymax,source_geometry_id\n";
  output << std::setprecision(17);
  for (std::size_t query_id = 0; query_id < options.query_count; ++query_id) {
    std::size_t source_id = distribution(rng);
    const geos::geom::Envelope* envelope = geometries[source_id]->getEnvelopeInternal();
    output << query_id << "," << envelope->getMinX() << "," << envelope->getMinY()
           << "," << envelope->getMaxX() << "," << envelope->getMaxY() << ","
           << source_id << "\n";
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

    write_queries(options, geometries);

    std::cout << "query_file=" << options.output_file
              << " lines_seen=" << lines_seen
              << " loaded=" << geometries.size()
              << " parse_errors=" << parse_errors
              << " query_count=" << options.query_count
              << " seed=" << options.seed
              << " load_ms=" << ns_count(load_end - load_start) / 1e6
              << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
