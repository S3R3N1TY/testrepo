#pragma once

#include <ecs/StructuralPhase.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class World;

enum class JournalOpType {
    CreateEntity,
    DestroyEntity,
    EmplaceComponent,
    SetComponent,
    RemoveComponent
};

enum class TransactionPhase {
    Prepare,
    Commit
};

struct FailureInjectionConfig {
    std::optional<uint64_t> failAtEntryId{};
    std::optional<size_t> failAfterNApply{};
    std::optional<TransactionPhase> failAtPhase{};
};

struct JournalEntry {
    uint64_t id{ 0 };
    JournalOpType type{ JournalOpType::SetComponent };
    StructuralPlaybackPhase phase{ StructuralPlaybackPhase::PostSim };
    std::vector<uint64_t> dependsOn{};
    std::function<bool(World&)> validate{};
    std::function<void(World&)> apply{};
    std::function<void(World&)> undo{};
};

class Transaction {
public:
    explicit Transaction(std::vector<JournalEntry> entries);

    [[nodiscard]] bool validateGraphAcyclic() const;
    void execute(World& world, const FailureInjectionConfig* failure = nullptr) const;

private:
    [[nodiscard]] std::vector<size_t> topoOrder() const;

    std::vector<JournalEntry> entries_{};
};
