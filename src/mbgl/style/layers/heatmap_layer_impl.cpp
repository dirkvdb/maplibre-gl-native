#include <mbgl/style/layers/heatmap_layer_impl.hpp>

namespace mbgl {
namespace style {

bool HeatmapLayer::Impl::hasLayoutDifference(const Layer::Impl& other) const {
    assert(other.getTypeInfo() == getTypeInfo());
    const auto& impl = static_cast<const style::HeatmapLayer::Impl&>(other);
    return filter != impl.filter || visibility != impl.visibility || paint.hasDataDrivenPropertyDifference(impl.paint);
}

} // namespace style
} // namespace mbgl
