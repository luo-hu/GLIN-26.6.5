// GEOS Quadtree baseline：读取同一份 WKT 数据和 query 文件，输出与 GLIN/Boost benchmark 相同格式的 CSV。
//
// 实验流程：
// 1. 读取 WKT 数据，解析为 GEOS Geometry。
// 2. 用每个 geometry 的 MBR 建 GEOS Quadtree。
// 3. 读取固定 query_file。
// 4. Quadtree 用 MBR 查询候选；GEOS 再做 contains/intersects 精确过滤。

#include <geos/geom/Coordinate.h>
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/LinearRing.h>
#include <geos/index/quadtree/Quadtree.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
using Quadtree = geos::index::quadtree::Quadtree;

struct Options {
  std::string data_file;
  std::string dataset_name = "WKT";
  std::string query_file;
  std::string output_csv;
  std::string relationship = "contains";
  std::size_t limit = 10000;
  std::uint64_t seed = 42;
};

struct QueryCase {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  Geometry* geometry = nullptr;
  geos::geom::Envelope envelope;
};

struct QueryResult {
  std::size_t query_id = 0;
  std::size_t source_geometry_id = 0;
  long long probe_ns = 0;
  long long refine_ns = 0;
  std::size_t candidates = 0;
  std::size_t answers = 0;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --data_file /path/to/AREAWATER.csv --query_file queries.csv [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME        Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N                  Number of valid geometries to load (default: 10000)\n"
      << "  --relationship contains    contains or intersects (default: contains)\n"
      << "  --output_csv PATH          Write per-query CSV rows\n"
      << "  --seed N                   Recorded in CSV for comparability (default: 42)\n";
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
    } else if (key == "--relationship") {
      options.relationship = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
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
  if (options.query_file.empty()) {
    throw std::runtime_error("--query_file is required");
  }
  if (options.limit == 0) {
    throw std::runtime_error("--limit must be greater than 0");
  }
  if (options.relationship != "contains" &&
      options.relationship != "intersects") {
    throw std::runtime_error("--relationship must be contains or intersects");
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
    query.source_geometry_id = static_cast<std::size_t>(std::stoull(fields[5]));
    query.envelope = geos::geom::Envelope(query.xmin, query.xmax,
                                          query.ymin, query.ymax);
    owned_queries.push_back(
        make_query_box(factory, query.xmin, query.ymin, query.xmax, query.ymax));
    query.geometry = owned_queries.back().get();
    queries.push_back(query);
  }

  if (queries.empty()) {
    throw std::runtime_error("No query rows loaded from: " + query_file);
  }
  return queries;
}

bool exact_match(const std::string& relationship, Geometry* query,
                 Geometry* candidate) {
  if (relationship == "contains") {
    return query->contains(candidate);
  }
  return query->intersects(candidate);
}

void build_quadtree(const std::vector<GeometryPtr>& geometries,
                    Quadtree& quadtree) {
  for (const auto& geometry : geometries) {
    quadtree.insert(geometry->getEnvelopeInternal(), geometry.get());
  }
}

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
            "cell_ymin,cell_size,seed,build_ns,query_id,source_geometry_id,"
            "probe_ns,refine_ns,total_ns,candidates,answers,visited_leaf,loaded_leaf\n";

  for (const auto& result : results) {
    output << options.dataset_name << ",GEOS_Quadtree," << options.relationship
           << "," << loaded_count << ",0,0,0,0," << options.seed << ","
           << build_ns << "," << result.query_id << ","
           << result.source_geometry_id << "," << result.probe_ns << ","
           << result.refine_ns << "," << (result.probe_ns + result.refine_ns)
           << "," << result.candidates << "," << result.answers << ",0,0\n";
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

    auto build_start = std::chrono::high_resolution_clock::now();
    Quadtree quadtree;
    build_quadtree(geometries, quadtree);
    auto build_end = std::chrono::high_resolution_clock::now();
    long long build_ns = ns_count(build_end - build_start);

    std::vector<GeometryPtr> owned_queries;
    std::vector<QueryCase> queries =
        load_query_file(options.query_file, *factory, owned_queries);

    std::vector<QueryResult> results;
    results.reserve(queries.size());

    for (const auto& query : queries) {
      std::vector<void*> candidates;

      auto probe_start = std::chrono::high_resolution_clock::now();
      quadtree.query(&query.envelope, candidates);
      auto probe_end = std::chrono::high_resolution_clock::now();

      auto refine_start = std::chrono::high_resolution_clock::now();
      std::size_t answers = 0;
      for (void* candidate : candidates) {
        Geometry* payload = static_cast<Geometry*>(candidate);
        if (exact_match(options.relationship, query.geometry, payload)) {
          ++answers;
        }
      }
      auto refine_end = std::chrono::high_resolution_clock::now();

      QueryResult result;
      result.query_id = query.query_id;
      result.source_geometry_id = query.source_geometry_id;
      result.probe_ns = ns_count(probe_end - probe_start);
      result.refine_ns = ns_count(refine_end - refine_start);
      result.candidates = candidates.size();
      result.answers = answers;
      results.push_back(result);
    }

    long long total_probe_ns = 0;
    long long total_refine_ns = 0;
    long long total_candidates = 0;
    long long total_answers = 0;
    for (const auto& result : results) {
      total_probe_ns += result.probe_ns;
      total_refine_ns += result.refine_ns;
      total_candidates += static_cast<long long>(result.candidates);
      total_answers += static_cast<long long>(result.answers);
    }

    write_csv(options.output_csv, options, geometries.size(), build_ns, results);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "dataset=" << options.dataset_name
              << " index=GEOS_Quadtree"
              << " relationship=" << options.relationship
              << " lines_seen=" << lines_seen
              << " loaded=" << geometries.size()
              << " parse_errors=" << parse_errors
              << " tree_size=" << quadtree.size()
              << " tree_depth=" << quadtree.depth()
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
