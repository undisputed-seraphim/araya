#pragma once

#include "medulla/service.hpp"

#include <map>
#include <memory>

namespace medulla {

class context : public std::enable_shared_from_this<context> {
public:
    explicit context(std::shared_ptr<context> parent = nullptr);

    static std::shared_ptr<context> root();

    std::shared_ptr<context> parent() const noexcept { return parent_; }

    std::shared_ptr<context> make_child();

    void bind(service_id id, binding b);

    void unbind(service_id id) noexcept;

    binding const* lookup(service_id id) const noexcept;

    binding* lookup_mutable(service_id id) noexcept;

    void set_metadata(service_id id, service_metadata metadata);

    service_metadata metadata_for(service_id id) const;

private:
    std::shared_ptr<context> parent_;
    std::map<owned_service_id, binding, transparent_id_less> bindings_;
    std::map<owned_service_id, service_metadata, transparent_id_less>
        metadata_;
};

}  // namespace medulla
