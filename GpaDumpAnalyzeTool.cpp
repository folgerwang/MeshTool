#include "assert.h"
#include <fstream>
#include <sstream>

#include "coremath.h"
#include "corefile.h"
#include "coretexture.h"
#include "glfunctionlist.h"
#include "GpaDumpAnalyzeTool.h"
#include <QMessageBox>
#include <QString>
#include "meshdata.h"
#include "debugout.h"

using namespace std;

string g_root_folder_name;
string g_shaders_folder_name;
string g_textures_folder_name;
string g_meshes_folder_name;
string g_materials_folder_name;

string g_primitive_name[10] = {
	"points",
	"lines",
	"lineloop",
	"linestrip",
	"triangles",
	"trianglestrip",
	"trianglefan",
	"quads",
	"quadstrip",
	"polygon"
};


uint32_t fill_uint(unsigned char* parse_ptr, uint32_t num_bytes = 4)
{
	uint32_t result = 0xdeadbeef;
	if (num_bytes > 0)
	{
		result = parse_ptr[0];
		if (num_bytes > 1)
			result |= (parse_ptr[1] << 8);
		if (num_bytes > 2)
			result |= (parse_ptr[2] << 16);
		if (num_bytes > 3)
			result |= (parse_ptr[3] << 24);
	}

	return result;
}

uint64_t fill_ulong(unsigned char* parse_ptr, uint32_t num_bytes = 8)
{
	uint64_t result = 0xdeadbeefdeadbeef;
	if (num_bytes > 0)
	{
		result = parse_ptr[0];
		if (num_bytes > 1)
			result |= ((uint64_t)parse_ptr[1] << 8);
		if (num_bytes > 2)
			result |= ((uint64_t)parse_ptr[2] << 16);
		if (num_bytes > 3)
			result |= ((uint64_t)parse_ptr[3] << 24);
		if (num_bytes > 4)
			result |= ((uint64_t)parse_ptr[4] << 32);
		if (num_bytes > 5)
			result |= ((uint64_t)parse_ptr[5] << 40);
		if (num_bytes > 6)
			result |= ((uint64_t)parse_ptr[6] << 48);
		if (num_bytes > 7)
			result |= ((uint64_t)parse_ptr[7] << 56);
	}

	return result;
}


int32_t fill_int(unsigned char* parse_ptr, uint32_t num_bytes = 4)
{
	uint32_t result = 0xdeadbeef;
	if (num_bytes > 0)
	{
		result = parse_ptr[0];
		if (num_bytes > 1)
			result |= (parse_ptr[1] << 8);
		if (num_bytes > 2)
			result |= (parse_ptr[2] << 16);
		if (num_bytes > 3)
			result |= (parse_ptr[3] << 24);
	}

	return *((int32_t*)&result);
}

uint16_t fill_ushort(unsigned char* parse_ptr, uint32_t num_bytes = 2)
{
	uint16_t result = 0xdead;
	if (num_bytes > 0)
	{
		result = parse_ptr[0];
		if (num_bytes > 1)
			result |= (parse_ptr[1] << 8);
	}

	return result;
}

uint32_t check_uint(unsigned char* parse_ptr, uint32_t num_bytes = 4)
{
	uint32_t result = fill_uint(parse_ptr, num_bytes);
	return result;
}

uint64_t check_ulong(unsigned char* parse_ptr, uint32_t num_bytes = 8)
{
	uint64_t result = fill_ulong(parse_ptr, num_bytes);
	return result;
}

int32_t check_int(unsigned char* parse_ptr, uint32_t num_bytes = 4)
{
	int32_t result = fill_int(parse_ptr, num_bytes);
	return result;
}

uint16_t check_ushort(unsigned char* parse_ptr, uint32_t num_bytes = 2)
{
	uint16_t result = fill_ushort(parse_ptr, num_bytes);
	return result;
}

float check_float(unsigned char* parse_ptr, uint32_t num_bytes = 4)
{
	uint32_t result = fill_uint(parse_ptr, num_bytes);
	return *(float*)&result;
}

uint32_t read_uint(unsigned char*& parse_ptr, uint32_t num_bytes = 4)
{
	uint32_t result = fill_uint(parse_ptr, num_bytes);
	parse_ptr+=4;

	return result;
}

struct CommandBlockInfo
{
	TagInfo			m_blockTag;
	uint32_t		m_numBytes;
	uint32_t		m_data1;
	uint32_t		m_data2;
	uint32_t		m_data3;
	uint32_t		m_data4;
	uint32_t		m_data5;
	uint32_t		m_data6;
	uint32_t		m_data7;
	uint32_t		m_data8;
	uint32_t		m_data9;
	uint32_t		m_data10;
	uint32_t		m_data11;
	uint32_t		m_data12;
	uint32_t		m_data13;
	uint32_t		m_data14;
	uint32_t		m_data15;
	uint32_t		m_data16;	
	uint32_t		m_offset;

	CommandBlockInfo() : m_data1(0xdeafbeef), 
						 m_data2(0xdeafbeef), 
						 m_data3(0xdeafbeef), 
						 m_data4(0xdeafbeef), 
						 m_data5(0xdeafbeef),
						 m_data6(0xdeafbeef),
						 m_data7(0xdeafbeef),
						 m_data8(0xdeafbeef),
						 m_data9(0xdeafbeef), 
						 m_data10(0xdeafbeef), 
						 m_data11(0xdeafbeef), 
						 m_data12(0xdeafbeef), 
						 m_data13(0xdeafbeef),
						 m_data14(0xdeafbeef),
						 m_data15(0xdeafbeef),
						 m_data16(0xdeafbeef)
	{}
};

void copy_line(char* dst_line, char* src_line, uint32_t num_pixels, uint32_t order[4])
{
	for (uint32_t i = 0; i < num_pixels; i++)
	{
		dst_line[i * 4 + 0] = src_line[i * 4 + order[0]];
		dst_line[i * 4 + 1] = src_line[i * 4 + order[1]];
		dst_line[i * 4 + 2] = src_line[i * 4 + order[2]];
		dst_line[i * 4 + 3] = src_line[i * 4 + order[3]];
	}
}

struct DataBlockInfo
{
	uint64_t	ref_tag;
	uint64_t	block_size;
	union
	{
		uint32_t*	int_data_address;
		float*		float_data_address;
		uint16_t*	short_data_address;
		char*		byte_data_address;
	};
};

struct DataZoneInfo
{
	TagInfo		tag_info;
	uint32_t	link_index;
	uint32_t	buffer_size;
	union
	{
		uint32_t*	int_data_address;
		float*		float_data_address;
		uint16_t*	short_data_address;
		char*		byte_data_address;
	};

    vector<DataBlockInfo> block_list;

	DataZoneInfo() { memset(this, 0, sizeof(DataZoneInfo)); }
};

struct CommandZoneInfo
{
	TagInfo		tag_info;
	uint32_t	buffer_size;
	union
	{
		uint32_t*	int_data_address;
		float*		float_data_address;
		uint16_t*	short_data_address;
		char*		byte_data_address;
	};
};

struct BufferDataInfo
{
	BufferType		buffer_type;
	uint32_t		buffer_obj_id;
	uint32_t		buffer_size;
	uint32_t		buffer_usage;
	union
	{
		uint32_t*	int_data_address;
		float*		float_data_address;
		uint16_t*	short_data_address;
		char*		byte_data_address;
	};
};

void Matrix4fToMatrix4d(const core::matrix4f& src_mat, core::matrix4d& dst_mat)
{
    core::vec4f row_0 = src_mat.get_row(0);
    core::vec4f row_1 = src_mat.get_row(1);
    core::vec4f row_2 = src_mat.get_row(2);
    core::vec4f row_3 = src_mat.get_row(3);
    dst_mat.set_row(0, core::vec4d(double(row_0.x), double(row_0.y), double(row_0.z), double(row_0.w)));
    dst_mat.set_row(1, core::vec4d(double(row_1.x), double(row_1.y), double(row_1.z), double(row_1.w)));
    dst_mat.set_row(2, core::vec4d(double(row_2.x), double(row_2.y), double(row_2.z), double(row_2.w)));
    dst_mat.set_row(3, core::vec4d(double(row_3.x), double(row_3.y), double(row_3.z), double(row_3.w)));
}

void CheckValidInternalFormat(uint32_t format)
{
	uint32_t base_internal_format[] = { kGlDepthComponent, kGlDepthStencil, kGlRed, kGlRg, kGlRgb, kGlRgba };
	uint32_t sized_internal_format[] = { kGlR8,	kGlR8SNorm,	kGlR16,	kGlR16SNorm, kGlRg8, kGlRg8SNorm, kGlRg16, kGlRg16SNorm, kGlR3G3B2,
										 kGlRgb4, kGlRgb5, kGlRgb8, kGlRgb8SNorm, kGlRgb10, kGlRgb12, kGlRgb16, kGlRgb16SNorm, kGlRgba2,
										 kGlRgba4, kGlRgb5A1, kGlRgba8, kGlRgba8SNorm, kGlRgb10A2, kGlRgb10A2UI, kGlRgba12, kGlRgba16,
										 kGlRgba16SNorm, kGlSrgb8, kGlSrgb8Alpha8, kGlR16F, kGlRg16F, kGlRgb16F, kGlRgba16F, kGlR32F,
										 kGlRg32F, kGlRgb32F, kGlRgba32F, kGlRg11FB10F, kGlRgb9E5, kGlR8I, kGlR8UI, kGlR16I, kGlR16UI,
										 kGlR32I, kGlR32UI, kGlRg8I, kGlRg8UI, kGlRg16I, kGlRg16UI, kGlRg32I, kGlRg32UI, kGlRgb8I,
										 kGlRgb8UI, kGlRgb16I, kGlRgb16UI, kGlRgb32I, kGlRgb32UI, kGlRgba8I, kGlRgba8UI, kGlRgba16I,
										 kGlRgba16UI, kGlRgba32I, kGlRgba32UI, kGlLuminance, kGlLuminanceAlpha, kGlAlpha8 };
	uint32_t compressed_internal_format[] = { kGlCmpsdRed, kGlCmpsdRg, kGlCmpsdRgb, kGlCmpsdRgba, kGlCmpsdSrgb, kGlCmpsdSrgbAlpha, kGlCmpsdRedRgtc1,
										 kGlCmpsdSignedRedRgtc1, kGlCmpsdRgRgtc2, kGlCmpsdSignedRgRgtc2, kGlCmpsdRgbaBptcUnorm,
										 kGlCmpsdSrgbAlphaBptcUnorm, kGlCmpsdRgbBptcSignedFloat, kGlCmpsdRgbBptcUnsignedFloat };

	bool found = false;
	for (int i = 0; i < sizeof(base_internal_format) / sizeof(uint32_t); i++)
	{
		if (format == base_internal_format[i])
			found = true;
	}
	
	if (!found)
	{
		for (int i = 0; i < sizeof(sized_internal_format) / sizeof(uint32_t); i++)
		{
			if (format == sized_internal_format[i])
				found = true;
		}
	}

	if (!found)
	{
		for (int i = 0; i < sizeof(compressed_internal_format) / sizeof(uint32_t); i++)
		{
			if (format == compressed_internal_format[i])
				found = true;
		}
	}

	assert(found);
}

void CheckValidFormat(uint32_t format)
{
	uint32_t valid_format_list[] = { kGlRed, kGlRg, kGlRgb, kGlBgr, kGlRgba, kGlBgra, kGlRedInteger, kGlRgInteger, kGlRgbInteger, kGlLuminance, kGlLuminanceAlpha,
								  kGlBgrInteger, kGlRgbaInteger, kGlBgraInteger, kGlStencilIndex, kGlDepthComponent, kGlDepthStencil, kGlAlpha };

	bool found = false;
	for (int i = 0; i < sizeof(valid_format_list) / sizeof(uint32_t); i++)
	{
		if (format == valid_format_list[i])
			found = true;
	}

	assert(found);
}

void CheckValidPixelType(uint32_t type)
{
	uint32_t void_type_list[] = { kGlUByte, kGlByte, kGlUShort, kGlShort, kGlUInt, kGlInt, kGlFloat, kGlUByte332, kGlUByte332Rev,
								 kGlUShort565, kGlUShort565Rev, kGlUShort4444, kGlUShort4444Rev, kGlUShort5551, kGlUShort5551Rev,
								 kGlUInt8888, kGlUInt8888Rev, kGlUInt1010102, kGlUInt1010102Rev };
	bool found = false;
	for (int i = 0; i < sizeof(void_type_list) / sizeof(uint32_t); i++)
	{
		if (type == void_type_list[i])
			found = true;
	}

	assert(found);
}

void CheckValidTextureParameterAction(uint32_t action)
{
	uint32_t void_action_list[] = { kGlTextureMagFilter, kGlTextureMinFilter, kGlTextureWrapS, kGlTextureWrapT, kGlTextureWrapRExt, kGlGenerateMipmap,
								  kGlGenerateMipmapHint, kGlTextureBaseLevel, kGlTextureMaxLevel, kGlTextureMinLod, kGlTextureMaxLod, kGlTextureBorderColor,
								  kGlTextureLodBias, kGlTextureCompareMode, kGlTextureCompareFunc, kGlTextureSwizzleR, kGlTextureSwizzleG, kGlTextureBorder,
								  kGlTextureSwizzleB, kGlTextureSwizzleA, kGlTextureSwizzleRgba, kGlDepthStencilTextureMode, kGlTextureMaxAnisotropyExt };
	bool found = false;
	for (int i = 0; i < sizeof(void_action_list) / sizeof(uint32_t); i++)
	{
		if (action == void_action_list[i])
			found = true;
	}

	assert(found);
}

void CheckValidTextureEnvMode(uint32_t mode)
{
	uint32_t void_mode_list[] = { kGlTextureEnvMode, kGlTextureEnvColor, kGlTextureLodBias, kGlCombineRgb, kGlCombineAlpha, kGlSrc0Rgb, kGlSrc1Rgb, kGlSrc2Rgb,
								  kGlSrc0Alpha, kGlSrc1Alpha, kGlSrc2Alpha, kGlOperand0Rgb, kGlOperand1Rgb, kGlOperand2Rgb, kGlOperand0Alpha,
								  kGlOperand1Alpha, kGlOperand2Alpha, kGlRgbScale, kGlAlphaScale, kGlCoordReplace };
	bool found = false;
	for (int i = 0; i < sizeof(void_mode_list) / sizeof(uint32_t); i++)
	{
		if (mode == void_mode_list[i])
			found = true;
	}

	assert(found);
}

void CheckValidTextureEnvOp(uint32_t op)
{
	uint32_t void_ops_list[] = { kGlAdd, kGlAddSigned, kGlInterpolate, kGlModulate, kGlDecal, kGlBlend, kGlReplace, kGlSubstract, kGlCombine,
								  kGlTexture, kGlConstant, kGlPrimaryColor, kGlPrevious, kGlSrcColor, kGlOneMinusSrcColor, kGlSrcAlpha,	kGlOneMinusSrcAlpha };
	bool found = false;
	for (int i = 0; i < sizeof(void_ops_list) / sizeof(uint32_t); i++)
	{
		if (op == void_ops_list[i])
			found = true;
	}

	assert(found);
}

void CheckValidBufferUsage(uint32_t usage)
{
	uint32_t void_usage_list[] = { kGlStreamDraw, kGlStreamRead, kGlStreamCopy, kGlStaticDraw, kGlStaticRead, kGlStaticCopy,
								   kGlDynamicDraw, kGlDynamicRead, kGlDynamicCopy };
	bool found = false;
	for (int i = 0; i < sizeof(void_usage_list) / sizeof(uint32_t); i++)
	{
		if (usage == void_usage_list[i])
			found = true;
	}

	assert(found);
}

DataZoneInfo FindDataZoneByIndex(const vector<DataZoneInfo>& data_zone_list, uint32_t data_index)
{
	if (data_index == 0)
	{
		return data_zone_list[0];
	}
	else
	{
		assert(data_index >= data_zone_list[0].link_index);
		uint32_t index = data_index - data_zone_list[0].link_index;
		assert(index < data_zone_list.size());
		assert(data_zone_list[index].link_index == data_index);
		return data_zone_list[index];
	}
}

const DataBlockInfo* FindDataZoneByVsTag(const vector<DataZoneInfo>& data_zone_list, uint32_t vertexstream_tag)
{
	if (vertexstream_tag != 0)
	{
        for (uint32_t i = 0; i < data_zone_list.size(); i++)
		{
			if (data_zone_list[i].tag_info == kVertexStreamBlock)
			{
                for (uint32_t j = 0; j < data_zone_list[i].block_list.size(); j++)
				{
					if ((data_zone_list[i].block_list[j].ref_tag & 0xffffffff) == vertexstream_tag)
					{
                        return &data_zone_list[i].block_list[j];
					}
				}
			}
		}
	}

    return nullptr;
}

uint32_t GetDataElementSize(uint32_t data_type)
{
	switch (data_type)
	{
	case kGlByte:
	case kGlUByte:
	case kGlUByte332:
	case kGlUByte332Rev:
		return 1;
	case kGlShort:
	case kGlUShort:
	case kGl2Bytes:
	case kGlUShort565:
	case kGlUShort565Rev:
	case kGlUShort4444:
	case kGlUShort4444Rev:
	case kGlUShort5551:
	case kGlUShort5551Rev:
		return 2;
	case kGl3Bytes:
		return 3;
	case kGlInt:
	case kGlUInt:
	case kGlFloat:
	case kGl4Bytes:
	case kGlUInt8888:
	case kGlUInt8888Rev:
	case kGlUInt1010102:
	case kGlUInt1010102Rev:
		return 4;
	case kGlDouble:
		return 8;
	}

	return 4;
}

enum BindBufferTypeList
{
	kGlArrayBufferIdx,
	kGlAtomicCounterBufferIdx,
	kGlCopyReadBufferIdx,
	kGlCopyWriteBufferIdx,
	kGlDispatchIndirectBufferIdx,
	kGlDrawIndirectBufferIdx,
	kGlElementArrayBufferIdx,
	kGlPixelPackBufferIdx,
	kGlPixelUnpackBufferIdx,
	kGlQueryBufferIdx,
	kGlShaderStorageBufferIdx,
	kGlTextureBufferIdx,
	kGlTransformFeedbackBufferIdx,
	kGlUniformBufferIdx,
	kGlNumBufferTypeIdx,
};

struct Vec2
{
	float	m_x;
	float	m_y;
};

struct Vec3
{
	float	m_x;
	float	m_y;
	float	m_z;
};

struct Vec4
{
	float	m_x;
	float	m_y;
	float	m_z;
	float	m_w;
};

struct VertexAttrib
{
	uint32_t	is_enabled;
	uint32_t	data_buffer_obj;
	uint32_t	num_elements;
	DataType	data_type;
	uint32_t	is_normalized;
	uint32_t	stride;
	uint32_t	start_offset;
};

struct DrawCallParameters
{
	uint32_t	element_buffer_obj;
	uint32_t	primitive_type;
	uint32_t	num_indexes;
	DataType	data_type;
	uint32_t	data_offset;
};

struct TextureSlotInfo
{
    uint32_t        index_in_list;
};

int32_t g_currentBindTexture = -1;
int32_t g_current_bind_buffer = -1; // for uploading data only.
int32_t g_bind_buffer_list[kGlNumBufferTypeIdx];

struct RenderingStates
{
	DrawCallParameters		draw_call_params;
	int32_t					binding_buffer_list[kGlNumBufferTypeIdx];
    VertexAttrib			m_vertexStream[256];
    TextureSlotInfo			m_textureSlot[256];
    core::matrix4f			m_transform_matrix;
    core::matrix4d			m_first_inv_transform_matrix;

	RenderingStates() 
	{ 
        for (int i = 0; i < 256; i++) m_textureSlot[i].index_in_list = uint32_t(-1);
        for (int i = 0; i < 16; i++) m_vertexStream[i].is_enabled = 0;
		for (int i = 0; i < kGlNumBufferTypeIdx; i++) binding_buffer_list[i] = -1;
	}
};

void CreateObjectFile(RenderingStates const& current_state, 
                    vector<core::matrix4f> matrix_stack,
                    const vector<BufferDataInfo>& buffer_data_list,
                    const vector<DataZoneInfo>& data_zone_list,
                    string obj_name,
                    string mtl_file_name,
					MeshData*& mesh_data)
{
    if (mtl_file_name == string("material_triangles_303.mtl")) {
        int hit = 1;
    }

	bool has_mesh_data_texture = current_state.m_vertexStream[0].is_enabled &&
								 current_state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1 &&
								 current_state.m_vertexStream[3].is_enabled &&
								 current_state.m_vertexStream[3].data_buffer_obj != (uint32_t)-1 &&
								 current_state.draw_call_params.data_type != (DataType)-1 &&
								 current_state.draw_call_params.element_buffer_obj != (uint32_t)-1;

	bool has_draw_data = current_state.m_vertexStream[0].is_enabled &&
						 (current_state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1 ||
						  current_state.m_vertexStream[0].start_offset > 0);

	bool is_line = current_state.draw_call_params.primitive_type == kGlLineStrip;

	if (has_mesh_data_texture || (has_draw_data && is_line))
	{
        int32_t pos_0 = int32_t(mtl_file_name.rfind('\\'));
        int32_t pos_1 = int32_t(mtl_file_name.rfind('/'));
        int32_t pos = max(pos_0, pos_1);
        string mtl_name = mtl_file_name.substr(uint32_t(pos + 1));

        vector<core::vec3f> position_data;
        vector<core::vec2f> texture_coord_data;
        vector<uint32_t> color_data;
        vector<uint16_t> index_data;
		position_data.reserve(10240);
		texture_coord_data.reserve(10240);
		color_data.reserve(10240);
		index_data.reserve(10240);

        uint32_t block_size = 0;

		if (current_state.m_vertexStream[0].is_enabled)
		{
			char* start_address = nullptr;
			char* end_address = nullptr;
			if (current_state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1)
			{
				BufferDataInfo data_info = buffer_data_list[current_state.m_vertexStream[0].data_buffer_obj];
				start_address = data_info.byte_data_address + current_state.m_vertexStream[0].start_offset;
				end_address = data_info.byte_data_address + data_info.buffer_size;
			}
			else
			{
				const DataBlockInfo* block_data = FindDataZoneByVsTag(data_zone_list, current_state.m_vertexStream[0].start_offset);
				if (block_data)
				{
					start_address = block_data->byte_data_address;
					end_address = block_data->byte_data_address + block_data->block_size;

                    block_size = block_data->block_size;
				}
			}

            while (start_address < end_address)
			{
				assert(current_state.m_vertexStream[0].num_elements == 3);
                float x = check_float(reinterpret_cast<uint8_t*>(start_address) + 4 * 0);
                float y = check_float(reinterpret_cast<uint8_t*>(start_address) + 4 * 1);
                float z = check_float(reinterpret_cast<uint8_t*>(start_address) + 4 * 2);

                position_data.push_back(core::vec3f(x, y, z));

				start_address += current_state.m_vertexStream[0].stride;
			}
		}

		if (current_state.m_vertexStream[3].is_enabled)
		{
            if (current_state.m_vertexStream[3].data_buffer_obj != uint32_t(-1))
			{
				BufferDataInfo data_info = buffer_data_list[current_state.m_vertexStream[3].data_buffer_obj];
				char* start_address = data_info.byte_data_address + current_state.m_vertexStream[3].start_offset;

                while (start_address < data_info.byte_data_address + data_info.buffer_size)
				{
                    core::vec2f uv;
                    for (int32_t i = 0; i < int32_t(current_state.m_vertexStream[3].num_elements); i++)
					{
                        uv[i] = check_float(reinterpret_cast<uint8_t*>(start_address) + 4 * i);
					}

					texture_coord_data.push_back(uv);
					start_address += current_state.m_vertexStream[3].stride;
				}
			}
		}

		if (is_line && current_state.m_vertexStream[2].is_enabled) // color channel.
		{
			char* start_address = nullptr;
			char* end_address = nullptr;
            if (current_state.m_vertexStream[2].data_buffer_obj != uint32_t(-1))
			{
				BufferDataInfo data_info = buffer_data_list[current_state.m_vertexStream[2].data_buffer_obj];
				start_address = data_info.byte_data_address + current_state.m_vertexStream[2].start_offset;
				end_address = data_info.byte_data_address + data_info.buffer_size;
			}
			else
			{
				const DataBlockInfo* block_data = FindDataZoneByVsTag(data_zone_list, current_state.m_vertexStream[2].start_offset);
				if (block_data)
				{
					start_address = block_data->byte_data_address;
					end_address = block_data->byte_data_address + block_data->block_size;
				}
			}

            while (start_address < end_address)
			{
				uint32_t color = 0;
				for (uint32_t i = 0; i < current_state.m_vertexStream[2].num_elements; i++)
				{
                    uint32_t value = (reinterpret_cast<uint8_t*>(start_address))[i];
					color |= (value << (8 * i));
				}
				color_data.push_back(color);
				start_address += current_state.m_vertexStream[2].stride;
			}
		}

        if (current_state.draw_call_params.data_type != DataType(-1)) // drawelements.
		{
			char* start_address = nullptr;
			char* end_address = nullptr;

            if (current_state.m_vertexStream[2].data_buffer_obj != uint32_t(-1))
			{
				BufferDataInfo data_info = buffer_data_list[current_state.draw_call_params.element_buffer_obj];
				start_address = data_info.byte_data_address + current_state.draw_call_params.data_offset;
				end_address = data_info.byte_data_address + data_info.buffer_size;
			}
			else
			{
				const DataBlockInfo* block_data = FindDataZoneByVsTag(data_zone_list, current_state.draw_call_params.data_offset);
				if (block_data)
				{
					start_address = block_data->byte_data_address;
					end_address = block_data->byte_data_address + block_data->block_size;
				}
			}

			if (current_state.draw_call_params.primitive_type == kGlTriangles)
			{
				for (uint32_t i = 0; i < current_state.draw_call_params.num_indexes; i += 3)
				{
                    uint16_t idx0 = check_ushort(reinterpret_cast<uint8_t*>(start_address));
                    uint16_t idx1 = check_ushort(reinterpret_cast<uint8_t*>(start_address + 2));
                    uint16_t idx2 = check_ushort(reinterpret_cast<uint8_t*>(start_address + 4));
					index_data.push_back(idx0);
					index_data.push_back(idx1);
					index_data.push_back(idx2);
					start_address += sizeof(uint16_t) * 3;
				}
			}
			else if (current_state.draw_call_params.primitive_type == kGlLineStrip)
			{
				for (uint32_t i = 0; i < current_state.draw_call_params.num_indexes; i ++)
				{
                    uint16_t idx0 = check_ushort(reinterpret_cast<uint8_t*>(start_address));
					index_data.push_back(idx0);
					start_address += sizeof(uint16_t);
				}
			}
		}

        vector<int32_t> index_match_table;
        vector<int32_t> new_index_match_table;
		index_match_table.reserve(position_data.size());
		new_index_match_table.reserve(position_data.size());
        for (uint32_t i = 0; i < position_data.size(); i++)
		{
			index_match_table.push_back(-1);
		}

        for (uint32_t i = 0; i < index_data.size(); i++)
		{
			if (index_data[i] < index_match_table.size() && index_match_table[index_data[i]] < 0)
			{
                index_match_table[index_data[i]] = int32_t(new_index_match_table.size());
				new_index_match_table.push_back(index_data[i]);
			}
		}

		if (new_index_match_table.size() > 0)
		{
			mesh_data = new MeshData;
            mesh_data->num_vertex = int32_t(new_index_match_table.size());
            uint32_t num_vertex = uint32_t(mesh_data->num_vertex);

            Matrix4fToMatrix4d(current_state.m_transform_matrix, mesh_data->dumpped_matrix);
            mesh_data->dumpped_matrix *= current_state.m_first_inv_transform_matrix;

			if (position_data.size() > 0)
			{
                mesh_data->vertex_list = make_unique<core::vec3f[]>(num_vertex);
                auto& vertex_list = mesh_data->vertex_list;
                for (uint32_t i = 0; i < num_vertex; i++)
				{
                    vertex_list[i] = position_data[uint32_t(new_index_match_table[i])];
                    assert(new_index_match_table[i] < position_data.size());
				}
            }

            if (texture_coord_data.size() > 0)
			{
                mesh_data->uv_list = make_unique<core::vec2f[]>(num_vertex);
                for (uint32_t i = 0; i < num_vertex; i++)
				{
                    mesh_data->uv_list[i] = texture_coord_data[uint32_t(new_index_match_table[i])];
					assert(new_index_match_table[i] < texture_coord_data.size());
				}
			}

			if (color_data.size() > 0)
			{
                mesh_data->color_list = make_unique<uint32_t[]>(num_vertex);
                for (uint32_t i = 0; i < num_vertex; i++)
				{
                    mesh_data->color_list[i] = color_data[uint32_t(new_index_match_table[i])];
					assert(new_index_match_table[i] < color_data.size());
				}
			}

			if (index_data.size() > 0)
			{
                if (is_line)
				{
                    mesh_data->add_draw_call_list(kGlLineStrip, int32_t(index_data.size()), int32_t(num_vertex));
                    DrawCallInfo& last_draw_call_info = mesh_data->get_last_draw_call_info();
                    for (uint32_t i = 0; i < index_data.size(); i++)
					{
						if (index_data[i] < index_match_table.size())
						{
                            last_draw_call_info.add_index(uint32_t(index_match_table[index_data[i]]));
						}
                    }
				}
				else
                {
                    mesh_data->add_draw_call_list(kGlTriangles, int32_t(index_data.size()), int32_t(num_vertex));
                    DrawCallInfo& last_draw_call_info = mesh_data->get_last_draw_call_info();
                    for (uint32_t i = 0; i < index_data.size(); i += 3)
					{
						if (index_data[i + 0] < index_match_table.size() &&
							index_data[i + 1] < index_match_table.size() &&
							index_data[i + 2] < index_match_table.size())
						{
                            last_draw_call_info.add_index(uint32_t(index_match_table[index_data[i + 0]]));
                            last_draw_call_info.add_index(uint32_t(index_match_table[index_data[i + 1]]));
                            last_draw_call_info.add_index(uint32_t(index_match_table[index_data[i + 2]]));
						}
					}
				}
			}
		}

        if (mesh_data)
        {
            mesh_data->idx_in_texture_list = current_state.m_textureSlot[0].index_in_list;
        }
	}
}

void CreateMeshFromDumpFile(const string& dump_file_name, GroupMeshData* group_mesh_data)
{
    vector<CommandZoneInfo> command_zone_list;
    vector<DataZoneInfo> data_zone_list;
    vector<BufferDataInfo> buffer_data_list;
    vector<core::matrix4f> matrix_stack;

	data_zone_list.reserve(0x10000);
	uint32_t num_dispatches = 0;

    uint32_t length = 0;
    uint8_t* buffer = reinterpret_cast<uint8_t*>(core::LoadFileToMemory(dump_file_name, length));
    bool mesh_dump_done = false;

	if (length > 0)
	{
        uint8_t* parse_ptr = buffer;
		uint32_t file_tag = read_uint(parse_ptr);

		if (file_tag == 0xaabbccdd)
		{
			uint32_t version_number = read_uint(parse_ptr);
            uint32_t counter = uint32_t(-1);

            while (!mesh_dump_done)
			{
                uint32_t offset = uint32_t(parse_ptr - buffer);
                assert(offset < length);

				uint32_t num_bytes = read_uint(parse_ptr);
				uint32_t block_tag = read_uint(parse_ptr);

				CommandBlockInfo this_info;
				this_info.m_offset = offset;
				this_info.m_numBytes = num_bytes;
                this_info.m_blockTag = TagInfo(block_tag);
				this_info.m_data1 = check_uint(parse_ptr, num_bytes);
				this_info.m_data2 = check_uint(parse_ptr + 4, num_bytes > 4 ? num_bytes - 4 : 0);
				this_info.m_data3 = check_uint(parse_ptr + 8, num_bytes > 8 ? num_bytes - 8 : 0);
				this_info.m_data4 = check_uint(parse_ptr + 12, num_bytes > 12 ? num_bytes - 12 : 0);
				this_info.m_data5 = check_uint(parse_ptr + 16, num_bytes > 16 ? num_bytes - 16 : 0);
				this_info.m_data6 = check_uint(parse_ptr + 20, num_bytes > 20 ? num_bytes - 20 : 0);
				this_info.m_data7 = check_uint(parse_ptr + 24, num_bytes > 24 ? num_bytes - 24 : 0);
				this_info.m_data8 = check_uint(parse_ptr + 28, num_bytes > 28 ? num_bytes - 28 : 0);

				uint32_t data_value = 0x37692;
				if (this_info.m_data1 == data_value ||
					this_info.m_data2 == data_value ||
					this_info.m_data3 == data_value ||
					this_info.m_data4 == data_value ||
					this_info.m_data5 == data_value ||
					this_info.m_data6 == data_value ||
					this_info.m_data7 == data_value ||
					this_info.m_data8 == data_value)
				{
				}
				// end of file parse.
				if (block_tag == 0xf0000000)
					break;

				if ((block_tag & 0xf0000000) == 0x50000000)
				{
				}
				else if ((block_tag & 0xf0000000) == 0x40000000)
				{
				}
				else if ((block_tag & 0xf0000000) == 0x30000000)
				{
					if (block_tag == 0x30001001)
					{
						uint32_t flag0 = check_uint(parse_ptr);
						uint32_t flag1 = check_uint(parse_ptr + 4);
						uint32_t flag2 = check_uint(parse_ptr + 8);
						uint32_t size = check_uint(parse_ptr + 12);
						uint32_t format = check_uint(parse_ptr + 16) & 0xffff;

						uint32_t w = size & 0xffff;
						uint32_t h = size >> 16;

						char* src_buffer = (char*)parse_ptr + 18;

                        core::ExportBmpImageFile(g_root_folder_name + "/capture.bmp", src_buffer, num_bytes - 18, w, h);
					}
				}
				// data zone.
				else if ((block_tag & 0xf0000000) == 0x20000000)
				{
					uint32_t stamp = check_uint(parse_ptr);

					if (counter == -1)					{
						counter = stamp;
					}
					else
					{
						assert((counter + 1) == stamp);
						counter = stamp;
					}

					DataZoneInfo data_info;
					const int header_size = 4;
					uint32_t zone_uint_size = (this_info.m_numBytes - header_size + 3) / 4;
					data_info.tag_info = (TagInfo)block_tag;
					data_info.link_index = stamp;
					data_info.buffer_size = this_info.m_numBytes - header_size;
					data_info.int_data_address = 0;

					if ((TagInfo)block_tag == kVertexStreamBlock)
					{
                        uint8_t* block_parse_ptr = parse_ptr + 4;
                        uint8_t* parse_end_ptr = parse_ptr + this_info.m_numBytes - header_size;
						while (block_parse_ptr < parse_end_ptr)
						{
							DataBlockInfo data_block_info;
							data_block_info.ref_tag = check_ulong(block_parse_ptr);
							data_block_info.block_size = check_ulong(block_parse_ptr + 8);
                            uint32_t num_uint = uint32_t((data_block_info.block_size + 3) / 4);
							data_block_info.int_data_address = new uint32_t[num_uint];
							memcpy(data_block_info.int_data_address, block_parse_ptr + 16, data_block_info.block_size);
                            uint32_t bytes_left = uint32_t(num_uint * 4 - data_block_info.block_size);
							memset((char*)data_block_info.int_data_address + data_block_info.block_size, 0, bytes_left);

							data_info.block_list.push_back(data_block_info);
							block_parse_ptr = block_parse_ptr + 16 + data_block_info.block_size;
						}
					}
                    else if (TagInfo(block_tag) == kArrayBufferBlock ||
                             TagInfo(block_tag) == kShaderBlock ||
                             TagInfo(block_tag) == kFrameBufferBlock ||
                             TagInfo(block_tag) == kTextureBufferBlock ||
                             TagInfo(block_tag) == kTextureBuffer1Block ||
                             TagInfo(block_tag) == kParamsBlock)
					{
						data_info.int_data_address = new uint32_t[zone_uint_size];
						memcpy(data_info.int_data_address, parse_ptr + header_size, data_info.buffer_size);
						uint32_t bytes_left = zone_uint_size * 4 - data_info.buffer_size;
						memset((char*)data_info.int_data_address + data_info.buffer_size, 0, bytes_left);
					}
					else if (block_tag == 0x20000c01 ||
							 block_tag == 0x20000c02 ||
							 block_tag == 0x20008002) // not used tag.
					{

					}
					else
					{
						assert(0);
					}
					data_zone_list.push_back(data_info);
				}
				// command zone.
				else if ((block_tag & 0xf0000000) == 0x10000000)
				{
					uint32_t zone_uint_size = (this_info.m_numBytes + 3) / 4;
					CommandZoneInfo data_info;
					data_info.tag_info = (TagInfo)block_tag;
					data_info.int_data_address = new uint32_t[zone_uint_size];
					data_info.buffer_size = this_info.m_numBytes;
					memcpy(data_info.int_data_address, parse_ptr, this_info.m_numBytes);
					uint32_t bytes_left = zone_uint_size * 4 - this_info.m_numBytes;
					memset((char*)data_info.int_data_address + this_info.m_numBytes, 0, bytes_left);
					command_zone_list.push_back(data_info);
				}
				else
				{
					assert(0);
				}

				parse_ptr += num_bytes;
			}
		}

		uint16_t* buffer_id_match_table = new uint16_t[0x10000];
		memset(buffer_id_match_table, 0xff, sizeof(uint16_t) * 10000);

		// data processing pass.
		for (uint32_t i = 0; i < command_zone_list.size(); i++)
		{
			TagInfo block_tag = command_zone_list[i].tag_info;
			uint8_t* parse_ptr = (uint8_t*)command_zone_list[i].byte_data_address;

			if (block_tag == kGlTexImage2D)
			{
                TextureType texture_type = TextureType(check_uint(parse_ptr));
				uint32_t level = check_uint(parse_ptr + 4);
				uint32_t internal_format = check_uint(parse_ptr + 8);
				CheckValidInternalFormat(internal_format);
				uint32_t width = check_uint(parse_ptr + 12);
				uint32_t height = check_uint(parse_ptr + 16);
				uint32_t border = check_uint(parse_ptr + 20);
				assert(border == 0);
                PixelDataChannel format = PixelDataChannel(check_uint(parse_ptr + 24));
				CheckValidFormat(format);
                DataType type = DataType(check_uint(parse_ptr + 28));
				CheckValidPixelType(type);
				uint32_t texture_address = check_uint(parse_ptr + 32);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, texture_address);
				assert(g_currentBindTexture >= 0);

                core::Texture2DInfo* tex_info = group_mesh_data->loaded_textures[uint32_t(g_currentBindTexture)];
                tex_info->m_levelCount = max(tex_info->m_levelCount, level + 1);
                tex_info->m_internalFormat = internal_format;
                tex_info->m_format = uint32_t(format);
                tex_info->m_type = uint32_t(type);
                tex_info->m_mips[level].m_width = width;
                tex_info->m_mips[level].m_height = height;
                tex_info->m_mips[level].m_size = data_zone_info.buffer_size;
                tex_info->m_mips[level].m_imageData = make_unique<char[]>(data_zone_info.buffer_size);
                memcpy(tex_info->m_mips[level].m_imageData.get(), data_zone_info.byte_data_address, data_zone_info.buffer_size);
			}
			else if (block_tag == kGlCompressedTexImage2D)
			{
                TextureType texture_type = TextureType(check_uint(parse_ptr));
				uint32_t level = check_uint(parse_ptr + 4);
				CompressedPixelFormat internal_compression_format = (CompressedPixelFormat)check_uint(parse_ptr + 8);
				uint32_t width = check_uint(parse_ptr + 12);
				uint32_t height = check_uint(parse_ptr + 16);
				uint32_t border = check_uint(parse_ptr + 20);
				assert(border == 0);
				uint32_t size = check_uint(parse_ptr + 24);
				uint32_t texture_address = check_uint(parse_ptr + 28);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, texture_address);
				assert(g_currentBindTexture >= 0);

                core::Texture2DInfo* tex_info = group_mesh_data->loaded_textures[uint32_t(g_currentBindTexture)];
                tex_info->m_levelCount = max(tex_info->m_levelCount, level + 1);
                tex_info->m_format = uint32_t(internal_compression_format);
                tex_info->m_internalFormat = uint32_t(internal_compression_format);
                tex_info->m_type = 0;
                tex_info->m_mips[level].m_width = width;
                tex_info->m_mips[level].m_height = height;
                tex_info->m_mips[level].m_size = data_zone_info.buffer_size;
                tex_info->m_mips[level].m_imageData = make_unique<char[]>(data_zone_info.buffer_size);
                memcpy(tex_info->m_mips[level].m_imageData.get(), data_zone_info.byte_data_address, data_zone_info.buffer_size);
			}
			else if (block_tag == kGlBufferData)
			{
				assert(g_current_bind_buffer > 0);
				assert(g_current_bind_buffer < 0x10000);

                BufferType buffer_type = BufferType(check_uint(parse_ptr));
				uint32_t buffer_size = check_uint(parse_ptr + 4);
				uint32_t buffer_addr = check_uint(parse_ptr + 8);
				uint32_t buffer_usage = check_uint(parse_ptr + 12);
				CheckValidBufferUsage(buffer_usage);

				if (buffer_addr > 0)
				{
					DataZoneInfo data_zone_info;
					data_zone_info = FindDataZoneByIndex(data_zone_list, buffer_addr);
					assert(buffer_size == data_zone_info.buffer_size);

                    buffer_id_match_table[g_current_bind_buffer] = uint16_t(buffer_data_list.size());

					BufferDataInfo this_buffer_info;
					this_buffer_info.buffer_type = buffer_type;
					this_buffer_info.buffer_size = buffer_size;
					this_buffer_info.buffer_usage = buffer_usage;
                    this_buffer_info.buffer_obj_id = uint32_t(g_current_bind_buffer);
					this_buffer_info.int_data_address = data_zone_info.int_data_address;
					buffer_data_list.push_back(this_buffer_info);
				}
			}
			else if (block_tag == kGlBufferSubData)
			{
				assert(g_current_bind_buffer > 0);
				assert(g_current_bind_buffer < 0x10000);

                BufferType buffer_type = BufferType(check_uint(parse_ptr));
				uint32_t buffer_offset = check_uint(parse_ptr + 4);
				uint32_t buffer_size = check_uint(parse_ptr + 8);
				uint32_t buffer_addr = check_uint(parse_ptr + 12);

				if (buffer_addr > 0)
				{
					DataZoneInfo data_zone_info;
					data_zone_info = FindDataZoneByIndex(data_zone_list, buffer_addr);
					assert(buffer_size == data_zone_info.buffer_size);

                    buffer_id_match_table[g_current_bind_buffer] = uint16_t(buffer_data_list.size());

					BufferDataInfo this_buffer_info;
					this_buffer_info.buffer_type = buffer_type;
					this_buffer_info.buffer_size = buffer_size;
					//this_buffer_info.buffer_usage = buffer_usage;
                    this_buffer_info.buffer_obj_id = uint32_t(g_current_bind_buffer);
					this_buffer_info.int_data_address = data_zone_info.int_data_address;
					buffer_data_list.push_back(this_buffer_info);
				}
			}
			else if (block_tag == kGlGenBuffers)
			{
				uint32_t num_buffers = check_uint(parse_ptr);
				uint32_t buffer_objects_address = check_uint(parse_ptr + 4);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, buffer_objects_address);
			}
			else if (block_tag == kGlGenTextures)
			{
				uint32_t num_textures = check_uint(parse_ptr);
				uint32_t texture_objects_address = check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlGenFrameBuffers)
			{
				uint32_t num_fbuffers = check_uint(parse_ptr);
				uint32_t framebuffer_objects_address = check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlBindBuffer)
			{
				BufferType buffer_type = (BufferType)check_uint(parse_ptr);
				uint32_t buffer_index = -1;
				uint32_t buffer_object_id = -1;
				if (block_tag == kGlBindBufferBase)
				{
					buffer_index = check_uint(parse_ptr + 4);
					buffer_object_id = check_uint(parse_ptr + 8);
				}
				else
				{
					buffer_object_id = check_uint(parse_ptr + 4);
				}

				g_current_bind_buffer = buffer_object_id;
			}
			else if (block_tag == kGlBindBufferBase || block_tag == kGlBindBuffersBase)
			{
				BufferType buffer_type = (BufferType)check_uint(parse_ptr);
				uint32_t buffer_index = -1;
				uint32_t buffer_object_id = -1;
				if (block_tag == kGlBindBufferBase)
				{
					buffer_index = check_uint(parse_ptr + 4);
					buffer_object_id = check_uint(parse_ptr + 8);
				}
				else
				{
					buffer_object_id = check_uint(parse_ptr + 4);
				}
			}
			else if (block_tag == kGlBindTexture)
			{
				TextureType texture_type = (TextureType)check_uint(parse_ptr);
				uint32_t texture_name_id = check_uint(parse_ptr + 4);

				int32_t found_texture = -1;
                for (uint32_t i = 0; i < group_mesh_data->loaded_textures.size(); i++)
				{
                    if (group_mesh_data->loaded_textures[i]->m_objectId == texture_name_id)
					{
						found_texture = i;
						break;
					}
				}

				if (found_texture < 0)
				{
                    core::Texture2DInfo* tex_info = new core::Texture2DInfo;
                    tex_info->m_objectId = texture_name_id;
                    tex_info->m_levelCount = 0;
                    group_mesh_data->loaded_textures.push_back(tex_info);
                    found_texture = int32_t(group_mesh_data->loaded_textures.size() - 1);
				}

				g_currentBindTexture = found_texture;
			}
			else if (block_tag == kGlBindTextures)
			{
				uint32_t start = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t texture_address = check_uint(parse_ptr + 8);

				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, texture_address);
			}
			else if (block_tag == kGlBindSampler)
			{
			}
			else if (block_tag == kGlBindSamplers)
			{
				uint32_t start = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t sampler_address = check_uint(parse_ptr + 8);
			}
			else if (block_tag == kGlBindFrameBuffer)
			{
				uint32_t frame_buffer_type = check_uint(parse_ptr);
				assert(frame_buffer_type == kGlFrameBuffer ||
					frame_buffer_type == kGlDrawFrameBuffer ||
					frame_buffer_type == kGlReadFrameBuffer);
			}
			else if (block_tag == kGlBindTransformFeedback)
			{
				uint32_t buffer_type = check_uint(parse_ptr);
				assert(buffer_type == kGlTransformFeedback);
			}
			else if (block_tag == kGlBindRenderBuffer)
			{
				uint32_t frame_buffer_type = check_uint(parse_ptr);
				assert(frame_buffer_type == kGlRenderBuffer);
			}
			else if (block_tag == kGlBindProgramPipeline)
			{
				uint32_t pipeline_id = check_uint(parse_ptr);
			}
			else if (block_tag == kGlPixelStoref || block_tag == kGlPixelStorei)
			{
				PixelStoreInfo pixel_store_info = (PixelStoreInfo)check_uint(parse_ptr);
			}
			else if (block_tag == kGlShaderSource)
			{
				uint32_t shader_obj = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t address_of_code_list = check_uint(parse_ptr + 8);
				uint32_t address_of_length_list = check_uint(parse_ptr + 12);
				DataZoneInfo code_zone_info = FindDataZoneByIndex(data_zone_list, address_of_code_list);
				if (address_of_length_list > 0)
				{
					DataZoneInfo length_zone_info = FindDataZoneByIndex(data_zone_list, address_of_length_list);
				}
				assert(count == 1);

                ostringstream s_name;
				s_name << g_shaders_folder_name << "\\shader_" << shader_obj << ".glsl";

                string file_name(s_name.str());
				ofstream outFile;
				outFile.open(file_name, ofstream::binary);
				outFile.write(code_zone_info.byte_data_address, code_zone_info.buffer_size);
				outFile.close();
			}
		}

		for (uint32_t i = 0; i < data_zone_list.size(); i++)
		{
			int32_t idx = (data_zone_list[i].link_index - data_zone_list[0].link_index);
			assert(idx == i);
		}
			
		g_current_bind_buffer = -1;
		memset(g_bind_buffer_list, 0xff, sizeof(g_bind_buffer_list));

		RenderingStates current_render_states;
        uint32_t currentTextureSlot = uint32_t(-1);
        MaxtrixMode current_matrix_mode = MaxtrixMode(-1);

        for (uint32_t i_cmd = 0; i_cmd < command_zone_list.size(); i_cmd++)
		{
			TagInfo block_tag = command_zone_list[i_cmd].tag_info;
            uint8_t* parse_ptr = reinterpret_cast<uint8_t*>(command_zone_list[i_cmd].byte_data_address);

			if (block_tag == kGlVertexAttribPointer ||
				block_tag == kGlVertexAttribIPointer ||
				block_tag == kGlVertexAttribLPointer)
			{
				uint32_t iterazer = 0;
				uint32_t attrib_index = check_uint(parse_ptr + iterazer); iterazer += 4;
				uint32_t num_elements = check_uint(parse_ptr + iterazer); iterazer += 4;
                DataType data_type = DataType(check_uint(parse_ptr + iterazer)); iterazer += 4;
				uint32_t is_normalized = 0;
				if (block_tag == kGlVertexAttribPointer)
				{
					is_normalized = check_uint(parse_ptr + iterazer); iterazer += 4;
				}
				uint32_t stride = check_uint(parse_ptr + iterazer); iterazer += 4;
				uint32_t data_offset = check_uint(parse_ptr + iterazer);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, data_offset);
                uint32_t element_size = GetDataElementSize(uint32_t(data_type));
				int currentBindBuffer = g_bind_buffer_list[kGlArrayBufferIdx];

				VertexAttrib& vertexAttrib = current_render_states.m_vertexStream[attrib_index];
				vertexAttrib.data_type = data_type;
				vertexAttrib.num_elements = num_elements;
				vertexAttrib.is_normalized = is_normalized;
				vertexAttrib.stride = stride == 0 ? num_elements * element_size : stride;
				vertexAttrib.start_offset = data_zone_info.int_data_address[0];
				if (currentBindBuffer > 0)
				{
					vertexAttrib.data_buffer_obj = buffer_id_match_table[currentBindBuffer] == 0xffff ? -1 : buffer_id_match_table[currentBindBuffer];
					assert(vertexAttrib.data_buffer_obj != -1);
				}
				else
				{
                    vertexAttrib.data_buffer_obj = uint32_t(-1);
				}
			}
			else if (block_tag == kGlEnableVertexAttribArray)
			{
				uint32_t attrib_index = check_uint(parse_ptr);
				assert(attrib_index < 16);
				current_render_states.m_vertexStream[attrib_index].is_enabled = 1;
			}
			else if (block_tag == kGlDisableVertexAttribArray)
			{
				uint32_t attrib_index = check_uint(parse_ptr);
				assert(attrib_index < 16);
				current_render_states.m_vertexStream[attrib_index].is_enabled = 0;
			}
			else if (block_tag == kGlMatrixMode)
			{
                MaxtrixMode matrix_mode = MaxtrixMode(check_uint(parse_ptr));
				assert(matrix_mode == kGlModelView ||
					matrix_mode == kGlProjection ||
					matrix_mode == kGlTexture ||
					matrix_mode == kGlColor);
				current_matrix_mode = matrix_mode;
			}
			else if (block_tag == kGlUseProgram)
			{
				uint32_t shader_address = check_uint(parse_ptr);
			}
			else if (block_tag == kGlTexImage2D ||
				block_tag == kGlCompressedTexImage2D ||
				block_tag == kGlGenBuffers ||
				block_tag == kGlBufferData ||
				block_tag == kGlGenTextures ||
				block_tag == kGlGenFrameBuffers ||
				block_tag == kGlShaderSource)
			{
				// done in previous pass.
			}
			else if (block_tag == kGlActiveTexture ||
				block_tag == kGlClientActiveTexture ||
				block_tag == kGlActiveTextureArb ||
				block_tag == kGlClientActiveTextureArb)
			{
                TextureSlot tex_slot = TextureSlot(check_uint(parse_ptr));

                int32_t slot_index = int32_t(tex_slot - kGlTexture0);
                assert(slot_index >= 0 && slot_index < 256);
				currentTextureSlot = slot_index;
			}
			else if (block_tag == kGlBindBuffer/* || block_tag == kGlBindBufferBase || block_tag == kGlBindBuffersBase*/)
			{
                BufferType buffer_type = BufferType(check_uint(parse_ptr));
                uint32_t buffer_index = uint32_t(-1);
				int32_t buffer_object_id = -1;
				if (block_tag == kGlBindBufferBase)
				{
					buffer_index = check_uint(parse_ptr + 4);
					buffer_object_id = check_int(parse_ptr + 8);
				}
				else
				{
					buffer_object_id = check_int(parse_ptr + 4);
				}

				BindBufferTypeList bufferTypeIdx = kGlArrayBufferIdx;
				switch (buffer_type)
				{
				case kGlArrayBuffer:
					bufferTypeIdx = kGlArrayBufferIdx;
					break;
				case kGlAtomicCounterBuffer:
					bufferTypeIdx = kGlAtomicCounterBufferIdx;
					break;
				case kGlCopyReadBuffer:
					bufferTypeIdx = kGlCopyReadBufferIdx;
					break;
				case kGlCopyWriteBuffer:
					bufferTypeIdx = kGlCopyWriteBufferIdx;
					break;
				case kGlDispatchIndirectBuffer:
					bufferTypeIdx = kGlDispatchIndirectBufferIdx;
					break;
				case kGlDrawIndirectBuffer:
					bufferTypeIdx = kGlDrawIndirectBufferIdx;
					break;
				case kGlElementArrayBuffer:
					bufferTypeIdx = kGlElementArrayBufferIdx;
					break;
				case kGlPixelPackBuffer:
					bufferTypeIdx = kGlPixelPackBufferIdx;
					break;
				case kGlPixelUnpackBuffer:
					bufferTypeIdx = kGlPixelUnpackBufferIdx;
					break;
				case kGlQueryBuffer:
					bufferTypeIdx = kGlQueryBufferIdx;
					break;
				case kGlShaderStorageBuffer:
					bufferTypeIdx = kGlShaderStorageBufferIdx;
					break;
				case kGlTextureBuffer:
					bufferTypeIdx = kGlTextureBufferIdx;
					break;
				case kGlTransformFeedbackBuffer:
					bufferTypeIdx = kGlTransformFeedbackBufferIdx;
					break;
				case kGlUniformBuffer:
                    bufferTypeIdx = kGlUniformBufferIdx;
					break;
				default:
                    assert(0);
				}

				g_bind_buffer_list[bufferTypeIdx] = buffer_object_id;
				current_render_states.binding_buffer_list[bufferTypeIdx] = buffer_object_id;
			}
			else if (block_tag == kGlBindTexture)
			{
                TextureType texture_type = static_cast<TextureType>(check_uint(parse_ptr));
				uint32_t texture_name_id = check_uint(parse_ptr + 4);

				int32_t found_texture = -1;
                for (uint32_t i = 0; i < group_mesh_data->loaded_textures.size(); i++)
				{
                    if (group_mesh_data->loaded_textures[i]->m_objectId == texture_name_id)
					{
                        found_texture = static_cast<int32_t>(i);
						break;
					}
				}

				if (found_texture < 0)
				{
                    core::Texture2DInfo* tex_info = new core::Texture2DInfo;
                    tex_info->m_objectId = texture_name_id;
                    tex_info->m_levelCount = 0;
                    group_mesh_data->loaded_textures.push_back(tex_info);
                    found_texture = int32_t(group_mesh_data->loaded_textures.size() - 1);
				}

				g_currentBindTexture = found_texture;

                assert(currentTextureSlot < 256);
                current_render_states.m_textureSlot[currentTextureSlot].index_in_list = found_texture;
            }
			else if (block_tag == kGlBindTextures)
			{
                uint32_t start = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t texture_address = check_uint(parse_ptr + 8);

				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, texture_address);
            }
			else if (block_tag == kGlBindSampler)
			{
			}
			else if (block_tag == kGlBindSamplers)
			{
				uint32_t start = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t sampler_address = check_uint(parse_ptr + 8);
			}
			else if (block_tag == kGlBindFrameBuffer)
			{
				uint32_t frame_buffer_type = check_uint(parse_ptr);
				assert(frame_buffer_type == kGlFrameBuffer ||
					frame_buffer_type == kGlDrawFrameBuffer ||
					frame_buffer_type == kGlReadFrameBuffer);
			}
			else if (block_tag == kGlBindTransformFeedback)
			{
				uint32_t buffer_type = check_uint(parse_ptr);
				assert(buffer_type == kGlTransformFeedback);
			}
			else if (block_tag == kGlBindRenderBuffer)
			{
				uint32_t frame_buffer_type = check_uint(parse_ptr);
				assert(frame_buffer_type == kGlRenderBuffer);
			}
			else if (block_tag == kGlBindProgramPipeline)
			{
				uint32_t pipeline_id = check_uint(parse_ptr);
			}
			else if (block_tag == kGlGetVertexAttribfv || block_tag == kGlGetVertexAttribiv)
			{
				uint32_t index = check_uint(parse_ptr);
				VertexAttribInfo vertex_attrib_info = (VertexAttribInfo)check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlMateriali ||
				block_tag == kGlMaterialiv ||
				block_tag == kGlMaterialf ||
				block_tag == kGlMaterialfv)
			{
				FaceInfo face_dir = (FaceInfo)check_uint(parse_ptr);
				MaterialLightAttrib material_attrib = (MaterialLightAttrib)check_uint(parse_ptr + 4);

				if (block_tag == kGlMaterialiv || block_tag == kGlMaterialfv)
				{
					uint32_t param_address = (MaterialLightAttrib)check_uint(parse_ptr + 8);
					DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, param_address);
				}
			}
			else if (block_tag == kGlLighti ||
				block_tag == kGlLightiv ||
				block_tag == kGlLightf ||
				block_tag == kGlLightfv)
			{
				LightIndex light_slot_idx = (LightIndex)check_uint(parse_ptr);
				MaterialLightAttrib material_attrib = (MaterialLightAttrib)check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlLightModeli ||
				block_tag == kGlLightModeliv ||
				block_tag == kGlLightModelf ||
				block_tag == kGlLightModelfv)
			{
				LightModel light_model = (LightModel)check_uint(parse_ptr);
			}
			else if (block_tag == kGlPointParameterf ||
				block_tag == kGlPointParameterfv ||
				block_tag == kGlPointParameteri ||
				block_tag == kGlPointParameteriv)
			{
				PointParams point_params = (PointParams)check_uint(parse_ptr);
			}
			else if (block_tag == kGlTexParameteri ||
				block_tag == kGlTexParameterf ||
				block_tag == kGlTexParameteriv ||
				block_tag == kGlTexParameterfv)
			{
				TextureType texture_type = (TextureType)check_uint(parse_ptr);
				TextureParameterAction texture_param_act = (TextureParameterAction)check_uint(parse_ptr + 4);

				CheckValidTextureParameterAction(texture_param_act);

				uint32_t texture_param_value = (uint32_t)-1;
				if (block_tag == kGlTexParameteri || block_tag == kGlTexParameterf)
					texture_param_value = (TextureParameterValue)check_uint(parse_ptr + 8);

				if (texture_param_value != (uint32_t)-1)
				{
					if (texture_param_act == kGlTextureMagFilter ||
						texture_param_act == kGlTextureMinFilter ||
						texture_param_act == kGlTextureWrapS ||
						texture_param_act == kGlTextureWrapT ||
						texture_param_act == kGlTextureWrapRExt)
					{
						assert(texture_param_value == kGlNearest ||
							texture_param_value == kGlLinear ||
							texture_param_value == kGlNearestMipmapNearest ||
							texture_param_value == kGlLinearMipmapNearest ||
							texture_param_value == kGlNearestMipmapLinear ||
							texture_param_value == kGlLinearMipmapLinear ||
							texture_param_value == kGlClamp ||
							texture_param_value == kGlRepeat ||
							texture_param_value == kGlClampToEdgeExt);
					}
					else if (texture_param_act == kGlGenerateMipmap)
					{
						assert(texture_param_value == kGlFalse || texture_param_value == kGlTrue);
					}
					else if (texture_param_act == kGlTextureBaseLevel ||
						texture_param_act == kGlTextureMaxLevel ||
						texture_param_act == kGlTextureMinLod ||
						texture_param_act == kGlTextureMaxLod ||
						texture_param_act == texture_param_act ||
						texture_param_act == kGlTextureMaxAnisotropyExt)
					{
					}
					else if (texture_param_act == kGlTextureCompareFunc)
					{
						assert(texture_param_value == kGlLess ||
							texture_param_value == kGlEqual ||
							texture_param_value == kGlLEqual ||
							texture_param_value == kGlGreater ||
							texture_param_value == kGlNotEqual ||
							texture_param_value == kGlGEqual ||
							texture_param_value == kGlAlways);
					}
					else if (texture_param_act == kGlTextureCompareMode)
					{
						assert(texture_param_value == kGlNone || texture_param_value == kGlCompareRToTexture);
					}
					else if (texture_param_act == kGlTextureSwizzleR ||
						texture_param_act == kGlTextureSwizzleG ||
						texture_param_act == kGlTextureSwizzleB ||
						texture_param_act == kGlTextureSwizzleA ||
						texture_param_act == kGlTextureSwizzleRgba)
					{
						assert(texture_param_value == kGlRed ||
							texture_param_value == kGlGreen ||
							texture_param_value == kGlBlue ||
							texture_param_value == kGlAlpha ||
							texture_param_value == kGlZero ||
							texture_param_value == kGlOne);
					}
					else if (texture_param_act == kGlDepthStencilTextureMode)
					{
						assert(texture_param_value == kGlDepthComponent);
					}
					else
					{
						assert(0);
					}
				}
			}
			else if (block_tag == kGlTexEnvi ||
				block_tag == kGlTexEnvf ||
				block_tag == kGlTexEnviv ||
				block_tag == kGlTexEnvfv)
			{
				TextureEnvMode tex_env_mode = (TextureEnvMode)check_uint(parse_ptr);
				assert(tex_env_mode == kGlTextureEnv || tex_env_mode == kGlTextureFilterControl || tex_env_mode == kGlPointSprite);
				TextureEnvParameters tex_env_param = (TextureEnvParameters)check_uint(parse_ptr + 4);
				CheckValidTextureEnvMode(tex_env_param);

				if (block_tag == kGlTexEnvi || block_tag == kGlTexEnvf)
				{
					if (tex_env_param == kGlTextureLodBias || tex_env_param == kGlRgbScale || tex_env_param == kGlAlphaScale)
					{
						float f_value = check_float(parse_ptr + 8);
					}
					else if (tex_env_param == kGlCoordReplace)
					{
						uint32_t flag = check_uint(parse_ptr + 8);
					}
					else
					{
						uint32_t ops = check_uint(parse_ptr + 8);
						CheckValidTextureEnvOp(ops);
					}
				}
			}
			else if (block_tag == kGlFogf || block_tag == kGlFogi || block_tag == kGlFogfv || block_tag == kGlFogiv)
			{
				FogFunc fog_func = (FogFunc)check_uint(parse_ptr);
			}
			else if (block_tag == kGlPixelStoref || block_tag == kGlPixelStorei)
			{
				PixelStoreInfo pixel_store_info = (PixelStoreInfo)check_uint(parse_ptr);
			}
			else if (block_tag == kGlClear)
			{
				bool is_clear_color = (check_uint(parse_ptr) & kGlColorBufferBit) != 0;
				bool is_clear_depth = (check_uint(parse_ptr) & kGlDepthBufferBit) != 0;
				bool is_clear_stencil = (check_uint(parse_ptr) & kGlStencilBufferBit) != 0;
				bool is_clear_accum = (check_uint(parse_ptr) & kGlAccumBufferBit) != 0;
				num_dispatches++;

                if (is_clear_stencil) {
                    mesh_dump_done = true;
                }
			}
			else if (block_tag == kGlClearColor ||
				block_tag == kGlClearDepth ||
				block_tag == kGlClearDepthf ||
				block_tag == kGlClearStencil)
			{

			}
			else if (block_tag == kGlStencilFunc ||
				block_tag == kGlStencilMask ||
				block_tag == kGlStencilOp)
			{
			}
			else if (block_tag == kGlDrawArrays)
			{
				PrimitiveType primitive_type = (PrimitiveType)check_uint(parse_ptr);
				uint32_t start_idx = check_uint(parse_ptr + 4);
				uint32_t count = check_uint(parse_ptr + 8);

				current_render_states.draw_call_params.element_buffer_obj = (uint32_t)-1;
				current_render_states.draw_call_params.primitive_type = primitive_type;
				current_render_states.draw_call_params.num_indexes = count;
				current_render_states.draw_call_params.data_type = (DataType)-1;
				current_render_states.draw_call_params.data_offset = start_idx;

                string prim_name = g_primitive_name[primitive_type];
                ostringstream obj_name, mtl_name;
				obj_name << g_meshes_folder_name << "/object_" << prim_name << "_" << num_dispatches << ".obj";
				mtl_name << g_materials_folder_name << "/material_" << prim_name << "_" << num_dispatches << ".mtl";

				MeshData* mesh_data = nullptr;
				CreateObjectFile(current_render_states, matrix_stack, buffer_data_list, data_zone_list, obj_name.str(), mtl_name.str(), mesh_data);
                if (mesh_data && !mesh_dump_done)
				{
                    group_mesh_data->meshes.push_back(mesh_data);
				}
					
				num_dispatches++;
			}
			else if (block_tag == kGlDrawElements)
			{
				PrimitiveType primitive_type = (PrimitiveType)check_uint(parse_ptr);
				uint32_t num_indexes = check_uint(parse_ptr + 4);
				DataType data_type = (DataType)check_uint(parse_ptr + 8);
				uint32_t data_offset_addr = check_uint(parse_ptr + 12);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, data_offset_addr);

				int32_t current_element_buffer_id = current_render_states.binding_buffer_list[kGlElementArrayBufferIdx];
				if (current_element_buffer_id <= 0) // no buffer bind.
				{
					current_render_states.draw_call_params.element_buffer_obj = -1;
					data_zone_info = FindDataZoneByIndex(data_zone_list, data_offset_addr);
				}
				else
				{
					current_render_states.draw_call_params.element_buffer_obj = buffer_id_match_table[current_element_buffer_id] == 0xffff ? -1 : buffer_id_match_table[current_element_buffer_id];
				}
				current_render_states.draw_call_params.primitive_type = primitive_type;
				current_render_states.draw_call_params.num_indexes = num_indexes;
				current_render_states.draw_call_params.data_type = data_type;
				current_render_states.draw_call_params.data_offset = data_zone_info.int_data_address[0];

                string prim_name = g_primitive_name[primitive_type];
                ostringstream obj_name, mtl_name;
				obj_name << g_meshes_folder_name << "/object_" << prim_name << "_" << num_dispatches << ".obj";
				mtl_name << g_materials_folder_name << "/material_" << prim_name << "_" << num_dispatches << ".mtl";

				MeshData* mesh_data = nullptr;
				CreateObjectFile(current_render_states, matrix_stack, buffer_data_list, data_zone_list, obj_name.str(), mtl_name.str(), mesh_data);
                if (mesh_data && !mesh_dump_done)
				{
                    group_mesh_data->meshes.push_back(mesh_data);
				}

				num_dispatches++;
			}
			else if (block_tag == kGlClipPlane)
			{
				uint32_t plane_idx = check_uint(parse_ptr);
				assert(plane_idx >= kGlClipPlane0 && plane_idx <= kGlClipPlane7);
				uint32_t address = check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlColorMask || block_tag == kGlColorMaski)
			{
			}
			else if (block_tag == kGlColorPointer ||
				block_tag == kGlTexCoordPointer ||
				block_tag == kGlNormalPointer ||
				block_tag == kGlVertexPointer ||
				block_tag == kGlIndexPointer)
			{
			}
			else if (block_tag == kGlUniform1fv ||
				block_tag == kGlUniform2fv ||
				block_tag == kGlUniform3fv ||
				block_tag == kGlUniform4fv ||
				block_tag == kGlUniform1iv ||
				block_tag == kGlUniform2iv ||
				block_tag == kGlUniform3iv ||
				block_tag == kGlUniform4iv)
			{
			}
			else if (block_tag == kGlUniformMatrix2fv ||
				block_tag == kGlUniformMatrix3fv ||
				block_tag == kGlUniformMatrix2x3fv ||
				block_tag == kGlUniformMatrix3x2fv ||
				block_tag == kGlUniformMatrix2x4fv ||
				block_tag == kGlUniformMatrix4x2fv ||
				block_tag == kGlUniformMatrix3x4fv ||
				block_tag == kGlUniformMatrix4x3fv)
			{
				uint32_t location = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t transpose = check_uint(parse_ptr + 8);
				uint32_t matrix_address = check_uint(parse_ptr + 12);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, matrix_address);
			}
			else if (block_tag == kGlUniformMatrix4fv)
			{
				uint32_t location = check_uint(parse_ptr);
				uint32_t count = check_uint(parse_ptr + 4);
				uint32_t transpose = check_uint(parse_ptr + 8);
				uint32_t matrix_address = check_uint(parse_ptr + 12);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, matrix_address);

                core::matrix4f shader_matrix;
                shader_matrix.set_row(0, core::vec4f(data_zone_info.float_data_address[0], data_zone_info.float_data_address[1], data_zone_info.float_data_address[2], data_zone_info.float_data_address[3]));
                shader_matrix.set_row(1, core::vec4f(data_zone_info.float_data_address[4], data_zone_info.float_data_address[5], data_zone_info.float_data_address[6], data_zone_info.float_data_address[7]));
                shader_matrix.set_row(2, core::vec4f(data_zone_info.float_data_address[8], data_zone_info.float_data_address[9], data_zone_info.float_data_address[10], data_zone_info.float_data_address[11]));
                shader_matrix.set_row(3, core::vec4f(data_zone_info.float_data_address[12], data_zone_info.float_data_address[13], data_zone_info.float_data_address[14], data_zone_info.float_data_address[15]));

                current_render_states.m_transform_matrix = shader_matrix;
                if (group_mesh_data->meshes.size() == 0) {
                    core::matrix4d tmp_matrix;
                    Matrix4fToMatrix4d(current_render_states.m_transform_matrix, tmp_matrix);
                    current_render_states.m_first_inv_transform_matrix = inverse(tmp_matrix);
                }

				matrix_stack.push_back(shader_matrix);
            }
			else if (block_tag == kGlVertexAttribP1ui ||
				block_tag == kGlVertexAttribP2ui ||
				block_tag == kGlVertexAttribP3ui ||
				block_tag == kGlVertexAttribP4ui ||
				block_tag == kGlVertexAttrib1f ||
				block_tag == kGlVertexAttrib2f ||
				block_tag == kGlVertexAttrib3f ||
				block_tag == kGlVertexAttrib4f ||
				block_tag == kGlVertexAttribI1i ||
				block_tag == kGlVertexAttribI2i ||
				block_tag == kGlVertexAttribI3i ||
				block_tag == kGlVertexAttribI4i ||
				block_tag == kGlVertexAttribI1ui ||
				block_tag == kGlVertexAttribI2ui ||
				block_tag == kGlVertexAttribI3ui ||
				block_tag == kGlVertexAttribI4ui ||
				block_tag == kGlVertexAttrib1s ||
				block_tag == kGlVertexAttrib2s ||
				block_tag == kGlVertexAttrib3s ||
				block_tag == kGlVertexAttrib4s ||
				block_tag == kGlVertexAttrib4Nub)
			{

			}
			else if (block_tag == kGlGetProgramResourceiv)
			{
				uint32_t program_idx = check_uint(parse_ptr);
				ProgramResourceInfo program_resource_info = (ProgramResourceInfo)check_uint(parse_ptr + 4);
				uint32_t inner_idx = check_uint(parse_ptr + 8);
				uint32_t prop_count = check_uint(parse_ptr + 12);
				uint32_t prop_array_address = check_uint(parse_ptr + 16);
				uint32_t buffer_size = check_uint(parse_ptr + 20);
				uint32_t length_address = check_uint(parse_ptr + 24);
				uint32_t params_address = check_uint(parse_ptr + 28);
			}
			else if (block_tag == kGlGetActiveUniformsiv)
			{
				uint32_t program_idx = check_uint(parse_ptr);
				uint32_t uniform_count = check_uint(parse_ptr + 4);
				uint32_t uniform_indices_address = check_uint(parse_ptr + 8);
				ProgramResourceInfo program_resource_info = (ProgramResourceInfo)check_uint(parse_ptr + 12);
				uint32_t params_address = check_uint(parse_ptr + 16);
			}
			else if (block_tag == kGlGetUniformLocation)
			{
				uint32_t result = check_uint(parse_ptr);
				uint32_t program_idx = check_uint(parse_ptr + 4);
				uint32_t name_address = check_uint(parse_ptr + 8);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, name_address);
			}
			else if (block_tag == kGlPushDebugGroup)
			{
				DebugResource debug_resource = (DebugResource)check_uint(parse_ptr);
				uint32_t id = check_uint(parse_ptr + 4);
				uint32_t length = check_uint(parse_ptr + 8);
				uint32_t messsage_addr = check_uint(parse_ptr + 12);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, messsage_addr);
			}
			else if (block_tag == kGlPopDebugGroup)
			{

			}
			else if (block_tag == kGlLoadMatrixf)
			{
				uint32_t matrix_address = check_uint(parse_ptr);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, matrix_address);
				if (current_matrix_mode == kGlModelView)
				{
                    //memcpy(current_render_states.m_transform_matrix, data_zone_info.int_data_address, sizeof(float) * 16);
				}
				else if (current_matrix_mode == kGlProjection)
				{
                    core::matrix4d p_m;
					float* m_data = data_zone_info.float_data_address;
                    p_m.set_row(0, core::vec4d(m_data[0], m_data[1], m_data[2], m_data[3]));
                    p_m.set_row(1, core::vec4d(m_data[4], m_data[5], m_data[6], m_data[7]));
                    p_m.set_row(2, core::vec4d(m_data[8], m_data[9], m_data[10], m_data[11]));
                    p_m.set_row(3, core::vec4d(m_data[12], m_data[13], m_data[14], m_data[15]));

                    //current_render_states.m_inv_projection_matrix = inverse(p_m);
				}
                else
				{
                    // skip all reset matrix.
				}
			}
			else if (block_tag == kGlHint)
			{
				HintInfo target = (HintInfo)check_uint(parse_ptr);
				HintMode mode = (HintMode)check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlDepthFunc)
			{
				CompareMode mode = (CompareMode)check_uint(parse_ptr);
			}
			else if (block_tag == kGlDepthMask)
			{
			}
			else if (block_tag == kGlColor3b ||
				block_tag == kGlColor3s ||
				block_tag == kGlColor3i ||
				block_tag == kGlColor3f ||
				block_tag == kGlColor3d ||
				block_tag == kGlColor3ub ||
				block_tag == kGlColor3us ||
				block_tag == kGlColor3ui ||
				block_tag == kGlColor4b ||
				block_tag == kGlColor4s ||
				block_tag == kGlColor4i ||
				block_tag == kGlColor4f ||
				block_tag == kGlColor4d ||
				block_tag == kGlColor4ub ||
				block_tag == kGlColor4us ||
				block_tag == kGlColor4ui)
			{

			}
			else if (block_tag == kGlColor3bv ||
				block_tag == kGlColor3sv ||
				block_tag == kGlColor3iv ||
				block_tag == kGlColor3fv ||
				block_tag == kGlColor3dv ||
				block_tag == kGlColor3ubv ||
				block_tag == kGlColor3usv ||
				block_tag == kGlColor3uiv ||
				block_tag == kGlColor4bv ||
				block_tag == kGlColor4sv ||
				block_tag == kGlColor4iv ||
				block_tag == kGlColor4fv ||
				block_tag == kGlColor4dv ||
				block_tag == kGlColor4ubv ||
				block_tag == kGlColor4usv ||
				block_tag == kGlColor4uiv)
			{

			}
			else if (block_tag == kGlVertex2s ||
				block_tag == kGlVertex2i ||
				block_tag == kGlVertex2f ||
				block_tag == kGlVertex2d ||
				block_tag == kGlVertex3s ||
				block_tag == kGlVertex3i ||
				block_tag == kGlVertex3f ||
				block_tag == kGlVertex3d ||
				block_tag == kGlVertex4s ||
				block_tag == kGlVertex4i ||
				block_tag == kGlVertex4f ||
				block_tag == kGlVertex4d)
			{

			}
			else if (block_tag == kGlVertex2sv ||
				block_tag == kGlVertex2iv ||
				block_tag == kGlVertex2fv ||
				block_tag == kGlVertex2dv ||
				block_tag == kGlVertex3sv ||
				block_tag == kGlVertex3iv ||
				block_tag == kGlVertex3fv ||
				block_tag == kGlVertex3dv ||
				block_tag == kGlVertex4sv ||
				block_tag == kGlVertex4iv ||
				block_tag == kGlVertex4fv ||
				block_tag == kGlVertex4dv)
			{

			}
			else if (block_tag == kGlNormal3b ||
				block_tag == kGlNormal3d ||
				block_tag == kGlNormal3f ||
				block_tag == kGlNormal3i ||
				block_tag == kGlNormal3s)
			{

			}
			else if (block_tag == kGlNormal3bv ||
				block_tag == kGlNormal3dv ||
				block_tag == kGlNormal3fv ||
				block_tag == kGlNormal3iv ||
				block_tag == kGlNormal3sv)
			{

			}
			else if (block_tag == kGlLogicOp)
			{
				LogicOpInfo logic_op_info = (LogicOpInfo)check_uint(parse_ptr);
			}
			else if (block_tag == kGlInvalidateBufferData ||
				block_tag == kGlInvalidateBufferSubData ||
				block_tag == kGlInvalidateFramebuffer ||
				block_tag == kGlActiveShaderProgram ||
				block_tag == kGlBindFragDataLocation ||
				block_tag == kGlGetFragDataLocation ||
				block_tag == kGlGetFragDataIndex ||
				block_tag == kGlBindAttribLocation)
			{

			}
			else if (block_tag == kGlGetShaderSource ||
				block_tag == kGlGetActiveAttrib ||
				block_tag == kGlGetActiveUniform)
			{

			}
			else if (block_tag == kGlGetAttribLocation)
			{
				uint32_t result = check_uint(parse_ptr);
				uint32_t program_id = check_uint(parse_ptr + 4);
				uint32_t name_address = check_uint(parse_ptr + 8);
				DataZoneInfo data_zone_info = FindDataZoneByIndex(data_zone_list, name_address);
			}
			else if (block_tag == kGlCreateShader)
			{
				uint32_t result = check_uint(parse_ptr);
				ShaderType shader_type = (ShaderType)check_uint(parse_ptr + 4);
			}
			else if (block_tag == kGlDeleteShader)
			{

			}
			else if (block_tag == kGlCreateProgram ||
				block_tag == kGlDeleteProgram ||
				block_tag == kGlCreateShaderProgramEXT ||
				block_tag == kGlCreateShaderProgramv ||
				block_tag == kGlGenRenderbuffers ||
				block_tag == kGlGenQueries ||
				block_tag == kGlDeleteQueries ||
				block_tag == kGlBeginQuery ||
				block_tag == kGlEndQuery ||
				block_tag == kGlAttachShader ||
				block_tag == kGlDetachShader ||
				block_tag == kGlBindVertexArray ||
				block_tag == kGlBindVertexBuffer ||
				block_tag == kGlBindVertexBuffers ||
				block_tag == kGlShadeModel ||
				block_tag == kGlCompileShader ||
				block_tag == kGlReleaseShaderCompiler ||
				block_tag == kGlRenderbufferStorage ||
				block_tag == kGlGetShaderiv ||
				block_tag == kGlGetShaderInfoLog ||
				block_tag == kGlBufferStorage)
			{

			}
			else if (block_tag == kGlVertexAttribFormat)
			{

			}
			else if (block_tag == kGlValidateProgram || block_tag == kGlValidateProgramPipeline)
			{

			}
			else if (block_tag == kGlPointSize || block_tag == kGlLineWidth)
			{

			}
			else if (block_tag == kGlBlendColor ||
				block_tag == kGlBlendEquation ||
				block_tag == kGlBlendFunc ||
				block_tag == kGlBlendEquationSeparate ||
				block_tag == kGlBlendFuncSeparate)
			{

			}
			else if (block_tag == kGlCullFace)
			{
				FaceInfo face_dir = (FaceInfo)check_uint(parse_ptr);
			}
			else if (block_tag == kGlFrontFace)
			{
				FaceOrientation face_orientation = (FaceOrientation)check_uint(parse_ptr);
                int hit = 1;
			}
			else if (block_tag == kGlAlphaFunc)
			{
				CompareMode alpha_func = (CompareMode)check_uint(parse_ptr);
				float alpha_cutoff = check_float(parse_ptr + 4);
			}
			else if (block_tag == kGlBegin)
			{
				PrimitiveType primitive_type = (PrimitiveType)check_uint(parse_ptr);
			}
			else if (block_tag == kGlEnd || block_tag == kGlFinish || block_tag == kGlFlush)
			{
			}
			else if (block_tag == kGlEnableClientState || block_tag == kGlDisableClientState)
			{
                ArrayType array_type = ArrayType(check_uint(parse_ptr));
			}
			else if (block_tag == kGlEnable || block_tag == kGlEnablei)
			{
				uint32_t op = check_uint(parse_ptr);
				//assert(op == kGlColorBufferBit);
			}
			else if (block_tag == kGlDisable || block_tag == kGlDisablei)
			{
				uint32_t op = check_uint(parse_ptr);
				if (op == kGlTexture1D || op == kGlTexture2D)
				{
                    current_render_states.m_textureSlot[currentTextureSlot].index_in_list = uint32_t(-1);
				}
			}
			else if (block_tag == kGlIsEnabled)
			{

			}
			else
			{
				int32_t total_num = sizeof(g_glFunctionsInfo) / sizeof(GlFunctionInfo);
                vector<string> name_list;
				for (int i = 0; i < total_num; i++)
				{
					uint32_t num_items = command_zone_list[i].buffer_size / 4;
					if (!g_glFunctionsInfo[i].m_used && g_glFunctionsInfo[i].m_numPointers == 0 && (g_glFunctionsInfo[i].m_numParameters + g_glFunctionsInfo[i].m_return) == num_items)
					{
						name_list.push_back(g_glFunctionsInfo[i].m_name);
					}
				}
			}
		}

        delete[] buffer;
    }
}




