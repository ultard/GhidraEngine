#ifndef GHIDRAENGINE_INDEX_MIH_PDQ_HPP
#define GHIDRAENGINE_INDEX_MIH_PDQ_HPP

#include <GhidraEngine/index/flat_pdq.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace GhidraEngine {

struct MihSearchStats {
    std::size_t slot_lookups{};
    std::size_t posting_visits{};
    std::size_t candidates_verified{};
    bool used_flat{};
};

class GHIDRAENGINE_EXPORT MihPdqIndex {
public:
    MihPdqIndex() = default;

    explicit MihPdqIndex(std::span<const PdqIndexEntry> entries);

    MihPdqIndex(const MihPdqIndex &) = default;

    MihPdqIndex(MihPdqIndex &&other) noexcept;

    MihPdqIndex &operator=(MihPdqIndex other) noexcept;
    ~MihPdqIndex() = default;

    void insert(FingerprintId id, const PdqHash &hash);

    void reserve(std::size_t capacity);

    [[nodiscard]] std::vector<PdqHit> search_within(
        const PdqHash &query,
        std::uint16_t max_distance,
        MihSearchStats *stats = nullptr
    ) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept;

private:
    using Links = std::array<std::uint32_t, 16>;
    std::vector<PdqIndexEntry> entries_;
    std::unordered_set<std::uint64_t> ids_;
    std::vector<Links> links_;
    std::vector<std::uint32_t> heads_;
    std::vector<std::uint32_t> counts_;

    void allocate_tables();

    void link(std::size_t position) noexcept;
    void swap(MihPdqIndex &other) noexcept;
};

}
#endif
