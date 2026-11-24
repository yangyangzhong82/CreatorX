#include "core/region/CuboidSelection.h"

#include <cmath>
#include <map>

namespace creatorx::core {
namespace {
struct Vector {
    double x, y, z;
    Vector operator-(Vector b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vector cross(Vector b) const { return {y*b.z-z*b.y,z*b.x-x*b.z,x*b.y-y*b.x}; }
    double dot(Vector b) const { return x*b.x+y*b.y+z*b.z; }
};
Vector vector(Vec3i p) { return {double(p.x),double(p.y),double(p.z)}; }
double side(Vec3i a, Vec3i b, Vec3i c, Vector p) {
    auto origin=vector(a);
    return (vector(b)-origin).cross(vector(c)-origin).dot(p-origin);
}
}

void CuboidSelection::setPoint(bool primary, Vec3i point) {
    if (type == SelectionType::cuboid) {
        (primary ? first : second) = point;
        return;
    }
    if (primary || !first) {
        first=point;
        second.reset();
        points={point};
        faces.clear();
        height.reset();
        radii={};
        return;
    }
    second=point;
    if (std::ranges::find(points,point)==points.end()) points.push_back(point);
    auto delta=point-*first;
    if (type==SelectionType::ellipsoid) {
        radii={std::max(radii.x,std::abs(delta.x)),std::max(radii.y,std::abs(delta.y)),std::max(radii.z,std::abs(delta.z))};
    } else if (type==SelectionType::sphere) {
        int radius=int(std::ceil(std::sqrt(double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z)));
        radii={radius,radius,radius};
    } else if (type==SelectionType::cyl) {
        int radius=int(std::ceil(std::hypot(double(delta.x),double(delta.z))));
        radii.x=radii.z=std::max(radii.x,radius);
    } else if (type==SelectionType::convex) rebuildHull();
}

bool CuboidSelection::complete() const {
    if (!first || !second) return false;
    if (type==SelectionType::convex) return !faces.empty();
    if (type==SelectionType::poly) {
        // 至少三个投影不共线的顶点；凹多边形也允许。
        for (size_t i=1;i<points.size();++i) for(size_t j=i+1;j<points.size();++j) {
            auto a=vector(points[i])-vector(points[0]);
            auto b=vector(points[j])-vector(points[0]);
            if (std::abs(a.x*b.z-a.z*b.x)>1e-6) return true;
        }
        return false;
    }
    return true;
}

std::optional<Box> CuboidSelection::bounds() const {
    if (!first || !second) return std::nullopt;
    if (type==SelectionType::cuboid) return Box::of(*first,*second);
    if (type==SelectionType::sphere || type==SelectionType::ellipsoid) return Box{*first-radii,*first+radii};
    auto box=Box::of(*first,*second);
    for (auto p:points) { box.min=Box::of(box.min,p).min; box.max=Box::of(box.max,p).max; }
    if(height) {box.min.y=height->first;box.max.y=height->second;}
    if (type==SelectionType::cyl) {
        box.min.x=first->x-radii.x; box.max.x=first->x+radii.x;
        box.min.z=first->z-radii.z; box.max.z=first->z+radii.z;
    }
    return box;
}

bool CuboidSelection::contains(Vec3i p) const {
    auto box=bounds();
    if (!box || !box->contains(p)) return false;
    if (type==SelectionType::cuboid) return true;
    if (type==SelectionType::sphere || type==SelectionType::ellipsoid || type==SelectionType::cyl) {
        auto d=p-*first;
        auto square=[](int d,int r) { double n=double(d)/(r+0.5); return n*n; };
        return square(d.x,radii.x)+square(d.z,radii.z)
            +(type==SelectionType::cyl ? 0 : square(d.y,radii.y))<=1.0;
    }
    if (type==SelectionType::convex) {
        if(faces.empty()) return false;
        return std::ranges::all_of(faces,[&](auto face) {
            return side(points[face[0]],points[face[1]],points[face[2]],vector(p))<=1e-6;
        });
    }
    bool inside=false;
    for (size_t i=0,j=points.size()-1;i<points.size();j=i++) {
        auto a=points[j],b=points[i];
        double cross=(double(p.x)-a.x)*(double(b.z)-a.z)-(double(p.z)-a.z)*(double(b.x)-a.x);
        if(std::abs(cross)<1e-6 && p.x>=std::min(a.x,b.x) && p.x<=std::max(a.x,b.x)
            && p.z>=std::min(a.z,b.z) && p.z<=std::max(a.z,b.z)) return true;
        if((a.z>p.z)!=(b.z>p.z) && double(p.x)<(double(b.x)-a.x)*(double(p.z)-a.z)/(double(b.z)-a.z)+a.x) inside=!inside;
    }
    return inside;
}

void CuboidSelection::shift(Vec3i offset) {
    if(first) *first=*first+offset;
    if(second) *second=*second+offset;
    for(auto& p:points) p=p+offset;
    if(height) {height->first+=offset.y;height->second+=offset.y;}
}

void CuboidSelection::rebuildHull() {
    faces.clear();
    if(points.size()<4) return;
    size_t b=1,c=points.size(),d=points.size();
    auto a=vector(points[0]);
    for(size_t i=2;i<points.size();++i) {
        auto normal=(vector(points[b])-a).cross(vector(points[i])-a);
        if(normal.dot(normal)>1e-6) {c=i;break;}
    }
    if(c==points.size()) return;
    for(size_t i=1;i<points.size();++i) if(std::abs(side(points[0],points[b],points[c],vector(points[i])))>1e-6) {d=i;break;}
    if(d==points.size()) return;
    Vector interior{(double(points[0].x)+points[b].x+points[c].x+points[d].x)/4,
                    (double(points[0].y)+points[b].y+points[c].y+points[d].y)/4,
                    (double(points[0].z)+points[b].z+points[c].z+points[d].z)/4};
    auto add=[&](size_t i,size_t j,size_t k) {
        if(side(points[i],points[j],points[k],interior)>0) std::swap(j,k);
        faces.push_back({i,j,k});
    };
    add(0,b,c); add(0,d,b); add(0,c,d); add(b,d,c);
    for(size_t i=1;i<points.size();++i) {
        if(i==b || i==c || i==d) continue;
        std::map<std::pair<size_t,size_t>,int> edges;
        std::erase_if(faces,[&](auto face) {
            if(side(points[face[0]],points[face[1]],points[face[2]],vector(points[i]))<=1e-6) return false;
            for(size_t k=0;k<3;++k) {
                auto u=face[k],v=face[(k+1)%3];
                ++edges[std::minmax(u,v)];
            }
            return true;
        });
        for(auto [edge,count]:edges) if(count==1) add(edge.first,edge.second,i);
    }
}
} // namespace creatorx::core
