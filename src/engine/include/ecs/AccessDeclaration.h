#pragma once

#include <cstdint>
#include <unordered_set>

struct AccessDeclaration {
    std::unordered_set<uint32_t> read{};
    std::unordered_set<uint32_t> write{};
};
