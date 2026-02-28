#include "GlbLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace {
struct JsonValue;
using JsonObject = std::unordered_map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    Storage value{};

    [[nodiscard]] const JsonObject& asObject() const { return std::get<JsonObject>(value); }
    [[nodiscard]] const JsonArray& asArray() const { return std::get<JsonArray>(value); }
    [[nodiscard]] const std::string& asString() const { return std::get<std::string>(value); }
    [[nodiscard]] double asNumber() const { return std::get<double>(value); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text)
        : text_(text)
    {
    }

    JsonValue parseValue()
    {
        skipWs();
        if (peek() == '{') return parseObject();
        if (peek() == '[') return parseArray();
        if (peek() == '"') return JsonValue{ parseString() };
        if (startsWith("true")) { pos_ += 4; return JsonValue{ true }; }
        if (startsWith("false")) { pos_ += 5; return JsonValue{ false }; }
        if (startsWith("null")) { pos_ += 4; return JsonValue{ nullptr }; }
        return JsonValue{ parseNumber() };
    }

private:
    JsonValue parseObject()
    {
        expect('{');
        JsonObject object{};
        skipWs();
        if (peek() == '}') { ++pos_; return JsonValue{ object }; }
        while (true) {
            const std::string key = parseString();
            skipWs();
            expect(':');
            object.insert_or_assign(key, parseValue());
            skipWs();
            if (peek() == '}') { ++pos_; break; }
            expect(',');
        }
        return JsonValue{ object };
    }

    JsonValue parseArray()
    {
        expect('[');
        JsonArray array{};
        skipWs();
        if (peek() == ']') { ++pos_; return JsonValue{ array }; }
        while (true) {
            array.push_back(parseValue());
            skipWs();
            if (peek() == ']') { ++pos_; break; }
            expect(',');
        }
        return JsonValue{ array };
    }

    std::string parseString()
    {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                const char e = text_[pos_++];
                if (e == '"' || e == '\\' || e == '/') out.push_back(e);
                else if (e == 'b') out.push_back('\b');
                else if (e == 'f') out.push_back('\f');
                else if (e == 'n') out.push_back('\n');
                else if (e == 'r') out.push_back('\r');
                else if (e == 't') out.push_back('\t');
                else throw std::runtime_error("Unsupported JSON escape sequence");
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    double parseNumber()
    {
        size_t consumed = 0;
        const double value = std::stod(std::string(text_.substr(pos_)), &consumed);
        pos_ += consumed;
        return value;
    }

    void skipWs()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void expect(char c)
    {
        skipWs();
        if (peek() != c) throw std::runtime_error("Invalid JSON content in GLB");
        ++pos_;
    }

    [[nodiscard]] bool startsWith(std::string_view s) const
    {
        return text_.substr(pos_, s.size()) == s;
    }

    [[nodiscard]] char peek() const
    {
        if (pos_ >= text_.size()) throw std::runtime_error("Unexpected end of JSON");
        return text_[pos_];
    }

    std::string_view text_;
    size_t pos_{ 0 };
};

template <typename T>
const JsonValue& expectField(const JsonObject& obj, const std::string& field)
{
    const auto it = obj.find(field);
    if (it == obj.end()) {
        throw std::runtime_error("Missing GLB JSON field: " + field);
    }
    return it->second;
}

uint32_t asU32(const JsonValue& value)
{
    return static_cast<uint32_t>(value.asNumber());
}

size_t componentSize(uint32_t componentType)
{
    switch (componentType) {
    case 5123: return sizeof(uint16_t);
    case 5125: return sizeof(uint32_t);
    case 5126: return sizeof(float);
    default: throw std::runtime_error("Unsupported GLB accessor componentType");
    }
}

size_t typeCount(const std::string& type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    throw std::runtime_error("Unsupported GLB accessor type");
}

struct AccessorView {
    uint32_t count{ 0 };
    uint32_t componentType{ 0 };
    std::string type{};
    uint32_t byteOffset{ 0 };
    uint32_t byteStride{ 0 };
};

AccessorView getAccessor(const JsonObject& root, const std::vector<uint8_t>& binChunk, uint32_t accessorIndex)
{
    const auto& accessors = expectField<JsonArray>(root, "accessors").asArray();
    const auto& bufferViews = expectField<JsonArray>(root, "bufferViews").asArray();
    const auto& accessor = accessors.at(accessorIndex).asObject();

    const uint32_t bufferViewIndex = asU32(expectField<JsonValue>(accessor, "bufferView"));
    const auto& view = bufferViews.at(bufferViewIndex).asObject();

    AccessorView result{};
    result.count = asU32(expectField<JsonValue>(accessor, "count"));
    result.componentType = asU32(expectField<JsonValue>(accessor, "componentType"));
    result.type = expectField<JsonValue>(accessor, "type").asString();

    const uint32_t accessorOffset = accessor.contains("byteOffset") ? asU32(accessor.at("byteOffset")) : 0;
    const uint32_t viewOffset = view.contains("byteOffset") ? asU32(view.at("byteOffset")) : 0;
    result.byteOffset = viewOffset + accessorOffset;

    const size_t elemBytes = componentSize(result.componentType) * typeCount(result.type);
    result.byteStride = view.contains("byteStride") ? asU32(view.at("byteStride")) : static_cast<uint32_t>(elemBytes);

    if (result.byteOffset + static_cast<uint64_t>(result.count - 1) * result.byteStride + elemBytes > binChunk.size()) {
        throw std::runtime_error("Accessor range exceeds GLB BIN chunk");
    }

    return result;
}

std::array<float, 3> readVec3(const std::vector<uint8_t>& binChunk, const AccessorView& view, uint32_t index)
{
    if (view.componentType != 5126 || view.type != "VEC3") {
        throw std::runtime_error("Only FLOAT VEC3 POSITION attributes are supported");
    }

    std::array<float, 3> out{};
    const size_t offset = view.byteOffset + static_cast<size_t>(index) * view.byteStride;
    std::memcpy(out.data(), binChunk.data() + offset, sizeof(float) * 3);
    return out;
}

std::array<float, 3> readColor(const std::vector<uint8_t>& binChunk, const AccessorView& view, uint32_t index)
{
    if (view.componentType != 5126 || (view.type != "VEC3" && view.type != "VEC4")) {
        throw std::runtime_error("Only FLOAT VEC3/VEC4 COLOR_0 attributes are supported");
    }

    std::array<float, 4> color{};
    const size_t offset = view.byteOffset + static_cast<size_t>(index) * view.byteStride;
    const size_t channelCount = (view.type == "VEC4") ? 4 : 3;
    std::memcpy(color.data(), binChunk.data() + offset, sizeof(float) * channelCount);
    return { color[0], color[1], color[2] };
}

uint32_t readIndex(const std::vector<uint8_t>& binChunk, const AccessorView& view, uint32_t index)
{
    const size_t offset = view.byteOffset + static_cast<size_t>(index) * view.byteStride;
    if (view.componentType == 5123) {
        uint16_t out{};
        std::memcpy(&out, binChunk.data() + offset, sizeof(out));
        return out;
    }
    if (view.componentType == 5125) {
        uint32_t out{};
        std::memcpy(&out, binChunk.data() + offset, sizeof(out));
        return out;
    }
    throw std::runtime_error("Only UNSIGNED_SHORT / UNSIGNED_INT indices are supported");
}
}

LoadedMesh appendGlbMeshVertices(const std::string& path, std::vector<VertexPacket>& outVertices)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Unable to open GLB file: " + path);
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Failed to read GLB file: " + path);
    }

    if (bytes.size() < 20) {
        throw std::runtime_error("Invalid GLB file (too small)");
    }

    const uint32_t magic = *reinterpret_cast<const uint32_t*>(bytes.data());
    const uint32_t version = *reinterpret_cast<const uint32_t*>(bytes.data() + 4);
    if (magic != 0x46546C67 || version != 2) {
        throw std::runtime_error("Only binary glTF 2.0 is supported");
    }

    size_t cursor = 12;
    const uint32_t jsonLength = *reinterpret_cast<const uint32_t*>(bytes.data() + cursor);
    const uint32_t jsonType = *reinterpret_cast<const uint32_t*>(bytes.data() + cursor + 4);
    cursor += 8;
    if (jsonType != 0x4E4F534A) {
        throw std::runtime_error("Missing JSON chunk in GLB file");
    }
    const std::string_view jsonText(reinterpret_cast<const char*>(bytes.data() + cursor), jsonLength);
    cursor += jsonLength;

    const uint32_t binLength = *reinterpret_cast<const uint32_t*>(bytes.data() + cursor);
    const uint32_t binType = *reinterpret_cast<const uint32_t*>(bytes.data() + cursor + 4);
    cursor += 8;
    if (binType != 0x004E4942) {
        throw std::runtime_error("Missing BIN chunk in GLB file");
    }
    std::vector<uint8_t> binChunk(binLength);
    std::memcpy(binChunk.data(), bytes.data() + cursor, binLength);

    const JsonObject root = JsonParser(jsonText).parseValue().asObject();
    const auto& meshes = expectField<JsonValue>(root, "meshes").asArray();
    const auto& mesh = meshes.at(0).asObject();
    const auto& primitive = expectField<JsonValue>(mesh, "primitives").asArray().at(0).asObject();
    const auto& attributes = expectField<JsonValue>(primitive, "attributes").asObject();

    const uint32_t positionAccessorIndex = asU32(expectField<JsonValue>(attributes, "POSITION"));
    const AccessorView positionAccessor = getAccessor(root, binChunk, positionAccessorIndex);

    const bool hasColorAttribute = attributes.contains("COLOR_0");
    const AccessorView colorAccessor = hasColorAttribute
        ? getAccessor(root, binChunk, asU32(attributes.at("COLOR_0")))
        : AccessorView{};

    const uint32_t firstVertex = static_cast<uint32_t>(outVertices.size());

    auto emitVertex = [&](uint32_t index) {
        const auto p = readVec3(binChunk, positionAccessor, index);
        const std::array<float, 3> color = hasColorAttribute
            ? readColor(binChunk, colorAccessor, index)
            : std::array<float, 3>{ 1.0F, 1.0F, 1.0F };
        outVertices.push_back(VertexPacket{ .position = p, .color = color });
    };

    if (primitive.contains("indices")) {
        const uint32_t indexAccessorIndex = asU32(primitive.at("indices"));
        const AccessorView indexAccessor = getAccessor(root, binChunk, indexAccessorIndex);
        for (uint32_t i = 0; i < indexAccessor.count; ++i) {
            emitVertex(readIndex(binChunk, indexAccessor, i));
        }
    } else {
        for (uint32_t i = 0; i < positionAccessor.count; ++i) {
            emitVertex(i);
        }
    }

    return LoadedMesh{
        .firstVertex = firstVertex,
        .vertexCount = static_cast<uint32_t>(outVertices.size()) - firstVertex,
    };
}
