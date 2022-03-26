#include <stdio.h>
#include "serial.hpp"
#include <iostream>
#include "projection.hpp"
#include "flatgeobuf/feature_generated.h"
#include "flatgeobuf/header_generated.h"
#include "milo/dtoa_milo.h"

using namespace std;

struct NodeItem {
    double minX;
    double minY;
    double maxX;
    double maxY;
    uint64_t offset;
};

// copied from https://github.com/flatgeobuf/flatgeobuf/blob/master/src/cpp/packedrtree.cpp#L365
uint64_t PackedRTreeSize(const uint64_t numItems, const uint16_t nodeSize)
{
    if (nodeSize < 2)
        throw std::invalid_argument("Node size must be at least 2");
    if (numItems == 0)
        throw std::invalid_argument("Number of items must be greater than 0");
    const uint16_t nodeSizeMin = std::min(std::max(nodeSize, static_cast<uint16_t>(2)), static_cast<uint16_t>(65535));
    // limit so that resulting size in bytes can be represented by uint64_t
    if (numItems > static_cast<uint64_t>(1) << 56)
        throw std::overflow_error("Number of items must be less than 2^56");
    uint64_t n = numItems;
    uint64_t numNodes = n;
    do {
        n = (n + nodeSizeMin - 1) / nodeSizeMin;
        numNodes += n;
    } while (n != 1);
    return numNodes * sizeof(NodeItem);
}

drawvec readGeometry(const FlatGeobuf::Geometry *geometry, FlatGeobuf::GeometryType geometry_type) {
	// if it is a GeometryCollection, parse Parts, ignore XY
	auto xy = geometry->xy();
	auto ends = geometry->ends();
	size_t current_end = 0;

	drawvec dv;
	for (unsigned int i = 0; i < xy->size(); i+=2) {
		long long x, y;
		projection->project(xy->Get(i), xy->Get(i+1), 32, &x, &y);
		if (i == 0 || (ends != nullptr && current_end < ends->size() && i == ends->Get(current_end)*2)) {
			dv.push_back(draw(VT_MOVETO, x, y));
			if (i > 0) current_end++;
		} else {
			dv.push_back(draw(VT_LINETO, x, y));
		}
	}

	return dv;
}

void parse_flatgeobuf(std::vector<struct serialization_state> *sst, const char *src, int layer, std::string layername) {
	auto header_size = flatbuffers::GetPrefixedSize((const uint8_t *)src + 8);
	auto header = FlatGeobuf::GetSizePrefixedHeader(src + 8);
	auto features_count = header->features_count();
	auto node_size = header->index_node_size();


	vector<string> h_column_names;
	vector<FlatGeobuf::ColumnType> h_column_types;
	for (size_t i = 0; i < header->columns()->size(); i++) {
		h_column_names.push_back(header->columns()->Get(i)->name()->c_str());
		h_column_types.push_back(header->columns()->Get(i)->type());
	}

	auto index_size = PackedRTreeSize(features_count,node_size);

	auto h_geometry_type = header->geometry_type();

	switch (h_geometry_type) {
		case FlatGeobuf::GeometryType::Unknown :
		case FlatGeobuf::GeometryType::Point :
		case FlatGeobuf::GeometryType::LineString :
		case FlatGeobuf::GeometryType::Polygon :
		case FlatGeobuf::GeometryType::MultiPoint :
		case FlatGeobuf::GeometryType::MultiLineString :
		case FlatGeobuf::GeometryType::MultiPolygon :
		case FlatGeobuf::GeometryType::GeometryCollection :
			break;
		default:
			fprintf(stderr, "flatgeobuf has unsupported geometry type\n");
			exit(EXIT_FAILURE);
	}

	const char* start = src + 8 + 4 + header_size + index_size;
	for (size_t i = 0; i < features_count; i++) {
		
		serial_feature sf;

		auto my_sst = &(*sst)[0];

		auto feature_size = flatbuffers::GetPrefixedSize((const uint8_t *)start);
		auto feature = FlatGeobuf::GetSizePrefixedFeature(start);

		drawvec dv = readGeometry(feature->geometry(), h_geometry_type);

		sf.layer = layer;
		sf.layername = layername;
		sf.segment = my_sst->segment;
		sf.has_id = false;
		sf.has_tippecanoe_minzoom = false;
		sf.has_tippecanoe_maxzoom = false;
		sf.feature_minzoom = false;
		sf.seq = (*my_sst->layer_seq);
		sf.geometry = dv;
		sf.t = 3;

		vector<string> full_keys;
		vector<serial_val> full_values;

		// assume tabular schema with columns in header
		size_t p_pos = 0;
		while (p_pos < feature->properties()->size()) {
			uint16_t col_idx;
			memcpy(&col_idx, feature->properties()->data() + p_pos, sizeof(col_idx));

			FlatGeobuf::ColumnType col_type = h_column_types[col_idx];

			serial_val sv;
			if (col_type == FlatGeobuf::ColumnType::Long) {
				sv.type = mvt_double; // this is quirky
				uint64_t long_val;
				memcpy(&long_val, feature->properties()->data() + p_pos + 2, sizeof(long_val));
				sv.s = to_string(long_val);
				p_pos += 2 + 8;
			} else if (col_type == FlatGeobuf::ColumnType::Double) {
				sv.type = mvt_double;
				double double_val;
				memcpy(&double_val, feature->properties()->data() + p_pos + 2, sizeof(double_val));
				sv.s = milo::dtoa_milo(double_val);
				p_pos += 2 + 8;
			} else if (col_type == FlatGeobuf::ColumnType::String) {
				sv.type = mvt_string;
				uint32_t str_len;
				memcpy(&str_len, feature->properties()->data() + p_pos + 2, sizeof(str_len));
				string s{reinterpret_cast<const char*>(feature->properties()->data() + p_pos + 2 + 4), str_len};
				sv.s = s;
				p_pos += 2 + 4 + str_len;
			} else {
				fprintf(stderr, "flatgeobuf has unsupported column type\n");
				exit(EXIT_FAILURE);
			}
			full_keys.push_back(h_column_names[col_idx]);
			full_values.push_back(sv);
		}

		sf.full_keys = full_keys;
		sf.full_values = full_values;

		serialize_feature(my_sst, sf);

		start += 4 + feature_size;
	}
}