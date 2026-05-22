#include "OceanMaskRasterizer.h"
#include <ogrsf_frmts.h>
#include <stdexcept>
#include <vector>

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

namespace bg  = boost::geometry;
namespace bgi = boost::geometry::index;

using BgPoint    = bg::model::point<double, 2, bg::cs::cartesian>;
using BgBox      = bg::model::box<BgPoint>;
using RtreeValue = std::pair<BgBox, std::size_t>;
using Rtree      = bgi::rtree<RtreeValue, bgi::rstar<16>>;

struct OceanMaskRasterizer::Impl {
    std::vector<OGRGeometry*> polygons;
    Rtree rtree;

    ~Impl() {
        for (auto* g : polygons)
            OGRGeometryFactory::destroyGeometry(g);
    }
};

OceanMaskRasterizer::OceanMaskRasterizer(const std::string& path)
    : impl_(std::make_unique<Impl>())
{
    GDALAllRegister();

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds)
        throw std::runtime_error("Cannot open NHD dataset: " + path);

    OGRLayer* layer = ds->GetLayerByName("NHDArea");
    if (!layer) {
        GDALClose(ds);
        throw std::runtime_error("NHDArea layer not found in: " + path);
    }

    layer->SetAttributeFilter("ftype = 445");

    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (geom) {
            OGRGeometry* clone = geom->clone();

            OGREnvelope env;
            clone->getEnvelope(&env);

            BgBox bbox(BgPoint(env.MinX, env.MinY), BgPoint(env.MaxX, env.MaxY));
            impl_->rtree.insert({bbox, impl_->polygons.size()});
            impl_->polygons.push_back(clone);
        }
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);
}

OceanMaskRasterizer::~OceanMaskRasterizer() = default;

bool OceanMaskRasterizer::is_ocean(double lat, double lon) const {
    BgPoint query_pt(lon, lat);

    std::vector<RtreeValue> candidates;
    impl_->rtree.query(bgi::intersects(query_pt), std::back_inserter(candidates));

    OGRPoint ogr_pt(lon, lat);
    for (const auto& [bbox, idx] : candidates) {
        if (impl_->polygons[idx]->Intersects(&ogr_pt))
            return true;
    }
    return false;
}

std::size_t OceanMaskRasterizer::polygon_count() const {
    return impl_->polygons.size();
}
