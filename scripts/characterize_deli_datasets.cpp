// Characterize a valid WKT prefix or full source without constructing an index.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../glin/Encoder.h"
#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>

namespace {
using Interval = std::pair<double, double>;

std::string extract_wkt(std::string line) {
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.empty()) return "";
  if (line.front() == '"') {
    const auto close = line.find('"', 1);
    if (close != std::string::npos) return line.substr(1, close - 1);
  }
  for (const auto& prefix : {"GEOMETRYCOLLECTION", "MULTIPOLYGON", "POLYGON",
                             "MULTILINESTRING", "LINESTRING", "MULTIPOINT", "POINT"}) {
    const auto start = line.find(prefix);
    if (start == std::string::npos) continue;
    const auto open = line.find('(', start);
    if (open == std::string::npos) return line.substr(start);
    int depth = 0;
    for (auto i = open; i < line.size(); ++i) {
      if (line[i] == '(') ++depth;
      if (line[i] == ')' && --depth == 0) return line.substr(start, i - start + 1);
    }
    return line.substr(start);
  }
  return "";
}

std::vector<std::uint64_t> interval_overlaps(const std::vector<Interval>& intervals) {
  std::vector<double> starts, ends;
  for (const auto& interval : intervals) {
    if (!std::isfinite(interval.first) || !std::isfinite(interval.second) ||
        interval.first > interval.second) throw std::runtime_error("Invalid interval");
    starts.push_back(interval.first);
    ends.push_back(interval.second);
  }
  std::sort(starts.begin(), starts.end());
  std::sort(ends.begin(), ends.end());
  std::vector<std::uint64_t> counts;
  for (const auto& interval : intervals) {
    const auto entered = std::upper_bound(starts.begin(), starts.end(), interval.second) - starts.begin();
    const auto left = std::lower_bound(ends.begin(), ends.end(), interval.first) - ends.begin();
    counts.push_back(static_cast<std::uint64_t>(entered - left - 1));
  }
  return counts;
}

std::uint64_t overlap_pairs(const std::vector<Interval>& intervals) {
  std::vector<double> starts, ends;
  starts.reserve(intervals.size());
  ends.reserve(intervals.size());
  for (const auto& interval : intervals) {
    if (!std::isfinite(interval.first) || !std::isfinite(interval.second) ||
        interval.first > interval.second) throw std::runtime_error("Invalid interval");
    starts.push_back(interval.first);
    ends.push_back(interval.second);
  }
  std::sort(starts.begin(), starts.end());
  std::sort(ends.begin(), ends.end());
  std::size_t expired = 0;
  std::uint64_t pairs = 0;
  for (std::size_t entered = 0; entered < starts.size(); ++entered) {
    while (expired < ends.size() && ends[expired] < starts[entered]) ++expired;
    pairs += entered - expired;
  }
  return pairs;
}

void self_test() {
  for (const std::vector<Interval>& intervals : {
           std::vector<Interval>{}, {{0, 1}}, {{0, 1}, {1, 2}},
           {{0, 10}, {1, 2}, {3, 4}, {20, 30}}, {{2, 2}, {2, 2}, {1, 3}},
           {{0, 1}, {0, 1}, {0, 0}, {1, 1}, {1.5, 2}}}) {
    const auto counts = interval_overlaps(intervals);
    if (overlap_pairs(intervals) * 2 !=
        std::accumulate(counts.begin(), counts.end(), std::uint64_t{0})) {
      throw std::runtime_error("Overlap pair-count self-test failed");
    }
    for (std::size_t i = 0; i < intervals.size(); ++i) {
      std::uint64_t expected = 0;
      for (std::size_t j = 0; j < intervals.size(); ++j) {
        if (i != j && intervals[i].first <= intervals[j].second &&
            intervals[j].first <= intervals[i].second) ++expected;
      }
      if (counts[i] != expected) throw std::runtime_error("Overlap self-test failed");
    }
  }
  if (extract_wkt("id,POLYGON ((0 0,1 0,1 1,0 0)),tail") !=
      "POLYGON ((0 0,1 0,1 1,0 0))" ||
      extract_wkt("\"LINESTRING (0 0,1 1)\"\tmetadata") != "LINESTRING (0 0,1 1)") {
    throw std::runtime_error("WKT extraction self-test failed");
  }
  std::cout << "Closed-interval overlap and WKT extraction self-tests passed\n";
}

void csv_text(std::ostream& stream, const std::string& value) {
  stream << '"';
  for (char c : value) {
    if (c == '"') stream << '"';
    stream << c;
  }
  stream << '"';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self_test") {
      self_test();
      return 0;
    }
    std::string input_path, output_path, dataset;
    std::size_t limit = 500000;
    bool full_dataset = false;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--full-dataset") {
        full_dataset = true;
        continue;
      }
      if (i + 1 == argc) throw std::runtime_error("Missing option value");
      const std::string value = argv[++i];
      if (key == "--data_file") input_path = value;
      else if (key == "--output_csv") output_path = value;
      else if (key == "--dataset_name") dataset = value;
      else if (key == "--limit") limit = std::stoull(value);
      else throw std::runtime_error("Unknown option: " + key);
    }
    if (input_path.empty() || output_path.empty() || dataset.empty() ||
        (!full_dataset && limit == 0)) {
      throw std::runtime_error("Require --data_file, --output_csv, --dataset_name and a positive limit unless --full-dataset is used");
    }
    std::ifstream input(input_path);
    if (!input) throw std::runtime_error("Cannot open " + input_path);
    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);
    Encoder<double> encoder(-180.0, 0.0000005, -90.0, 0.0000005);
    geos::geom::Envelope bounds;
    std::vector<Interval> intervals;
    if (!full_dataset) intervals.reserve(limit);
    std::map<std::string, std::size_t> types;
    std::size_t lines = 0, errors = 0, empty = 0;
    std::uint64_t vertices = 0;
    long double area_sum = 0, span_sum = 0;
    std::string line;
    while ((full_dataset || intervals.size() < limit) && std::getline(input, line)) {
      ++lines;
      const auto wkt = extract_wkt(line);
      if (wkt.empty()) continue;
      std::unique_ptr<geos::geom::Geometry> geometry;
      try {
        geometry = reader.read(wkt);
      } catch (const std::exception&) {
        ++errors;
        continue;
      }
      if (!geometry || geometry->isEmpty()) {
        ++empty;
        continue;
      }
      const auto* env = geometry->getEnvelopeInternal();
      const double x0 = env->getMinX(), x1 = env->getMaxX();
      const double y0 = env->getMinY(), y1 = env->getMaxY();
      if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) ||
          !std::isfinite(y1) || x0 < -180 || x1 > 180 || y0 < -90 || y1 > 90) {
        throw std::runtime_error("Nonfinite or out-of-domain MBR at line " + std::to_string(lines));
      }
      const auto encoded = encoder.encode_z(x0, y0, x1, y1);
      const Interval interval{static_cast<double>(encoded.first), static_cast<double>(encoded.second)};
      if (interval.first > interval.second) throw std::runtime_error("Reversed Z interval");
      intervals.push_back(interval);
      vertices += geometry->getNumPoints();
      ++types[geometry->getGeometryType()];
      bounds.expandToInclude(env);
      area_sum += static_cast<long double>(x1 - x0) * (y1 - y0);
      span_sum += static_cast<long double>(interval.second) - interval.first;
      if (intervals.size() % 100000 == 0) {
        std::cerr << dataset << ": " << intervals.size() << " objects\n";
      }
    }
    if (!full_dataset && intervals.size() != limit) {
      throw std::runtime_error("Dataset shorter than requested valid prefix");
    }
    if (intervals.size() < 2) throw std::runtime_error("Need at least two valid nonempty objects");
    const double zlo = std::min_element(intervals.begin(), intervals.end(),
        [](const Interval& a, const Interval& b) { return a.first < b.first; })->first;
    const double zhi = std::max_element(intervals.begin(), intervals.end(),
        [](const Interval& a, const Interval& b) { return a.second < b.second; })->second;
    const long double domain_area = static_cast<long double>(bounds.getWidth()) * bounds.getHeight();
    const long double domain_span = static_cast<long double>(zhi) - zlo;
    if (domain_area <= 0 || domain_span <= 0) throw std::runtime_error("Degenerate prefix domain");
    std::uint64_t overlap_sum = 0;
    std::uint64_t p95_overlap = 0;
    if (full_dataset) {
      overlap_sum = overlap_pairs(intervals) * 2;
    } else {
      auto overlaps = interval_overlaps(intervals);
      overlap_sum = std::accumulate(overlaps.begin(), overlaps.end(), std::uint64_t{0});
      std::sort(overlaps.begin(), overlaps.end());
      p95_overlap = overlaps[static_cast<std::size_t>(std::ceil(0.95 * overlaps.size())) - 1];
    }
    std::ofstream output(output_path);
    if (!output) throw std::runtime_error("Cannot open " + output_path);
    output << "dataset,data_file,loaded_count,lines_seen,parse_errors,empty_geometries,geometry_types,"
              "coordinate_count,avg_vertices,mean_normalized_mbr_area,mean_normalized_sfc_span,"
              "mean_interval_overlaps,p95_interval_overlaps,pair_overlap_fraction,"
              "bbox_minx,bbox_miny,bbox_maxx,bbox_maxy,zmin,zmax,cell_xmin,cell_ymin,cell_size\n";
    csv_text(output, dataset);
    output << ',';
    csv_text(output, input_path);
    output << ',' << intervals.size() << ',' << lines << ',' << errors << ',' << empty << ',';
    std::string type_text;
    for (const auto& type : types) {
      if (!type_text.empty()) type_text += ';';
      type_text += type.first + ":" + std::to_string(type.second);
    }
    csv_text(output, type_text);
    const long double n = intervals.size();
    output << std::setprecision(17) << ',' << vertices << ',' << vertices / n << ','
           << area_sum / n / domain_area << ',' << span_sum / n / domain_span << ','
           << overlap_sum / n << ',';
    if (!full_dataset) output << p95_overlap;
    output << ',' << overlap_sum / n / (n - 1) << ','
           << bounds.getMinX() << ',' << bounds.getMinY() << ',' << bounds.getMaxX() << ','
           << bounds.getMaxY() << ',' << zlo << ',' << zhi << ",-180,-90,0.0000005\n";
    if (!output) throw std::runtime_error("Failed writing statistics");
    std::cerr << dataset << ": complete; parse errors=" << errors << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return 1;
  }
}
