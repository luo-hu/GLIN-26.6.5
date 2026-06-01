#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <limits>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cctype>

// GEOS 几何库
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/Polygon.h>
#include <geos/geom/MultiPolygon.h>
#include <geos/geom/Envelope.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

// 自定义GLIN编码头文件
#include "glin/glin.h"

// ===================== 【配置区 - 统一修改这里，无需改动业务代码】 =====================
const std::string INPUT_CSV_PATH  = "/mnt/hgfs/AREAWATER.csv";
const std::string OUTPUT_BIN_PATH = "./areawater_binary";
const double GEO_XMIN    = -180.0;
const double GEO_YMIN    = -90.0;
const double CELL_X_INTV = 0.0001;
const double CELL_Y_INTV = 0.0001;

// 曲线类型枚举（替代字符串，避免拼写错误）
enum class CurveType { Z_CURVE, H_CURVE };
const CurveType CURVE_TYPE = CurveType::Z_CURVE;
// =======================================================================================

/**
 * @brief 移除字符串首尾空白、制表、换行、空格
 */
void trim_string(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](int ch) { return !std::isspace(static_cast<unsigned char>(ch)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](int ch) { return !std::isspace(static_cast<unsigned char>(ch)); }).base(), s.end());
}

/**
 * @brief 移除UTF-8 BOM（\xEF\xBB\xBF）
 */
void remove_utf8_bom(std::string& s)
{
    if (s.size() >= 3 &&
        static_cast<uint8_t>(s[0]) == 0xEF &&
        static_cast<uint8_t>(s[1]) == 0xBB &&
        static_cast<uint8_t>(s[2]) == 0xBF)
    {
        s = s.substr(3);
    }
}

/**
 * @brief 【修复版】括号层级计数提取标准 WKT
 * 修复点：1. 先剥离CSV外层双引号  2. 遍历时跳过引号+空白  3. 后置清理零星引号
 */
std::string extract_valid_wkt(std::string line)
{
    // 1. 基础清洗：BOM、Windows换行符、首尾空白
    remove_utf8_bom(line);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    trim_string(line);
    if (line.empty())
        return "";

    // ========== 关键修复1：优先剥离 CSV 外层成对双引号（标准CSV包裹格式） ==========
    if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
    {
        line = line.substr(1, line.size() - 2);
        trim_string(line);
    }

    size_t str_len = line.size();
    int bracket_level = 0;
    size_t wkt_end_pos = std::string::npos;

    // ========== 关键修复2：遍历同时跳过 空白 + 双引号，引号不参与括号计数 ==========
    for (size_t i = 0; i < str_len; ++i)
    {
        char c = line[i];
        unsigned char uc = static_cast<unsigned char>(c);

        // 跳过空白 和 双引号
        if (std::isspace(uc) || c == '"')
            continue;

        if (c == '(')
        {
            bracket_level++;
        }
        else if (c == ')')
        {
            bracket_level--;
            // 括号层级归0：WKT 语法正式结束
            if (bracket_level == 0)
            {
                wkt_end_pos = i;
                break;
            }
            // 非法括号嵌套，直接判定失败
            if (bracket_level < 0)
                return "";
        }
    }

    // 未找到合法闭合括号，提取失败
    if (wkt_end_pos == std::string::npos)
        return "";

    // 截断：仅保留完整WKT本体，丢弃后方所有脏数据/附加字段
    std::string wkt = line.substr(0, wkt_end_pos + 1);

    // ========== 关键修复3：二次清理首尾残留的引号、空白 ==========
    trim_string(wkt);
    // 清理开头单个引号
    while (!wkt.empty() && wkt.front() == '"')
        wkt.erase(wkt.begin());
    // 清理结尾单个引号
    while (!wkt.empty() && wkt.back() == '"')
        wkt.pop_back();
    trim_string(wkt);

    return wkt;
}

/**
 * @brief 从WKT字符串构建几何对象（支持 POLYGON / MULTIPOLYGON）
 * @return 智能指针，自动释放内存
 */
std::unique_ptr<geos::geom::Geometry> create_geometry_from_wkt(
    const std::string& wkt,
    geos::geom::GeometryFactory& factory)
{
    geos::io::WKTReader reader(&factory);
    try
    {
        return reader.read(wkt);
    }
    catch (const geos::util::GEOSException& e)
    {
        std::cerr << "[GEOS解析异常] " << e.what()
                  << " | 行内WKT片段: " << wkt.substr(0, 200) << "..." << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[通用异常] " << e.what()
                  << " | 行内WKT片段: " << wkt.substr(0, 200) << "..." << std::endl;
    }
    catch (...)
    {
        std::cerr << "[未知异常] 解析WKT失败" << std::endl;
    }
    return nullptr;
}

/**
 * @brief 几何外包盒映射为曲线编码区间 [start, end]
 * 返回 bool：true=计算成功，false=无效Envelope
 */
bool geometry_to_curve_range(
    const geos::geom::Envelope* env,
    CurveType curve_type,
    Encoder<double>& encoder,
    double& dist_start,
    double& dist_end)
{
    if (!env)
        return false;

    double minX = env->getMinX();
    double minY = env->getMinY();
    double maxX = env->getMaxX();
    double maxY = env->getMaxY();

    std::pair<double, double> range;
    if (curve_type == CurveType::H_CURVE)
    {
        range = encoder.encode_h(minX, minY, maxX, maxY);
    }
    else // Z_CURVE
    {
        range = encoder.encode_z(minX, minY, maxX, maxY);
    }

    dist_start = range.first;
    dist_end   = range.second;
    return true;
}

int main()
{
    // 全局复用对象（仅创建一次，提升大文件性能）
    auto geo_factory = geos::geom::GeometryFactory::create();
    std::unique_ptr<Encoder<double>> encoder =
        std::make_unique<Encoder<double>>(GEO_XMIN, CELL_X_INTV, GEO_YMIN, CELL_Y_INTV);

    // 打开输入文件
    std::ifstream input_file(INPUT_CSV_PATH);
    if (!input_file.is_open())
    {
        std::cerr << "错误：无法打开文件 " << INPUT_CSV_PATH << std::endl;
        return 1;
    }

    std::vector<double> curve_addrs;

    // 分层统计变量
    uint64_t total_lines     = 0;  // 文件总行数
    uint64_t extract_fail    = 0;  // WKT提取失败行数
    uint64_t geom_success    = 0;  // 几何解析成功数
    uint64_t geom_fail       = 0;  // 几何解析失败数
    uint64_t multi_poly_cnt  = 0;  // MULTIPOLYGON 数量
    uint64_t valid_range_cnt = 0;  // 有效编码区间数量

    std::string line;
    while (std::getline(input_file, line))
    {
        total_lines++;

        std::string wkt = extract_valid_wkt(line);
        if (wkt.empty())
        {
            extract_fail++;
            std::cerr << "[行号:" << total_lines << "] WKT提取失败，跳过该行" << std::endl;
            continue;
        }

        auto geom = create_geometry_from_wkt(wkt, *geo_factory);
        if (!geom)
        {
            geom_fail++;
            std::cerr << "[行号:" << total_lines << "] 几何解析失败" << std::endl;
            continue;
        }
        geom_success++;

        // 统计 MULTIPOLYGON
        if (geom->getGeometryTypeId() == geos::geom::GEOS_MULTIPOLYGON)
        {
            multi_poly_cnt++;
        }

        const geos::geom::Envelope* env = geom->getEnvelopeInternal();
        double s = 0.0, e = 0.0;
        bool calc_ok = geometry_to_curve_range(env, CURVE_TYPE, *encoder, s, e);
        if (!calc_ok)
        {
            std::cerr << "[行号:" << total_lines << "] 几何外包盒无效，跳过" << std::endl;
            continue;
        }

        curve_addrs.push_back(s);
        curve_addrs.push_back(e);
        valid_range_cnt++;
    }
    input_file.close();

    // 写入二进制文件
    std::ofstream output_file(OUTPUT_BIN_PATH, std::ios::binary);
    if (!output_file.is_open())
    {
        std::cerr << "错误：无法创建输出文件 " << OUTPUT_BIN_PATH << std::endl;
        return 1;
    }

    for (double val : curve_addrs)
    {
        output_file.write(reinterpret_cast<const char*>(&val), sizeof(double));
    }
    output_file.close();

    // 统计输出
    std::cout << "==================== 处理完成 统计信息 ====================" << std::endl;
    std::cout << "文件总行数:        " << total_lines << std::endl;
    std::cout << "WKT提取失败行数:   " << extract_fail << std::endl;
    std::cout << "几何解析成功数:    " << geom_success << std::endl;
    std::cout << "几何解析失败数:    " << geom_fail << std::endl;
    std::cout << "MULTIPOLYGON数量:  " << multi_poly_cnt << std::endl;
    std::cout << "有效编码区间数:    " << valid_range_cnt << std::endl;
    std::cout << "写入 double 总数:  " << curve_addrs.size() << std::endl;
    std::cout << "输出文件路径:      " << OUTPUT_BIN_PATH << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}