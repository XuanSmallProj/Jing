#include "VolIntegrator.h"

#include <memory>
#include <vector>

#include "CoreLayer/ColorSpace/RGB.h"
#include "CoreLayer/ColorSpace/Spectrum.h"
#include "CoreLayer/Math/Geometry.h"
#include "FunctionLayer/Integrator/Integrator.h"
#include "FunctionLayer/Light/Light.h"
#include "FunctionLayer/Material/BxDF/BSDF.h"
#include "FunctionLayer/Material/Material.h"
#include "FunctionLayer/Medium/Medium.h"
#include "FunctionLayer/Ray/Ray.h"
#include "FunctionLayer/Shape/Intersection.h"

struct ShadowRay {
    Vector3f wi;
    Spectrum spec;
};

Spectrum directLighting(const Scene &scene, const Intersection &its,
                        const LightSampleResult &res, float sample_pdf,
                        Medium *medium);

Spectrum VolIntegrator::li(Ray &ray, const Scene &scene,
                           std::shared_ptr<Sampler> sampler) const {
    auto itsOpt = scene.rayIntersect(ray);
    Spectrum L(0.0f);

    Spectrum throughput(1.0f);
    /*
    如果是specular，那么brdf就是一个delta函数，在tracing过程中统计光源的影响的时候就会落下，
    因此需要对这种情况特殊处理。
    */
    bool specularBounce = false;
    Medium *medium = ray.medium;

    int depth = 0;
    while (true) {
        if (!itsOpt.has_value()) {
            // todo: environment light
            break;
        }

        Intersection its;
        SurfaceIntersection sit = itsOpt.value();

        if (depth == 0 || specularBounce) {
            auto light = sit.shape->light;
            if (light) {
                Spectrum lighting =
                    light->evaluateEmission(sit, -ray.direction);
                Spectrum tr = scene.Tr(ray);
                L += throughput * tr * lighting;
            }
        }

        its = sit;
        bool hit = true;
        MediumIntersection mit;
        medium = ray.medium;
        if (medium) {
            medium->sample(ray, sit, mit);
            throughput *= mit.beta;

            if (mit.distance < ray.tFar) {
                hit = false;
                its = mit;
            }
        }

        ++depth;

        Vector3f wo = -ray.direction;
        std::shared_ptr<BSDF> bsdf = nullptr;
        if (hit) {
            bsdf = sit.shape->material->computeBSDF(sit);
        }

        // 计算光源的贡献
        for (auto light : scene.infiniteLights) {
            auto res = light->sample(its, sampler->next2D());
            Medium *init_medium = nullptr;
            Vector3f wi = res.direction;
            if (hit) {
                init_medium = sit.getMedium(wi);
            } else {
                init_medium = medium;
            }
            Spectrum lightSpec =
                directLighting(scene, its, res, 1.0f, init_medium);
            if (!lightSpec.isZero()) {
                if (hit) {
                    Spectrum f = bsdf->f(wo, wi);
                    L += throughput * f * lightSpec;
                } else {
                    float p = medium->phase.p(wo, wi);
                    L += throughput * p * lightSpec;
                }
            }
        }
        float pdfLight = .0f;
        auto light = scene.sampleLight(sampler->next1D(), &pdfLight);
        if (pdfLight > 0 && light) {
            auto res = light->sample(its, sampler->next2D());
            Medium *init_medium = nullptr;
            Vector3f wi = res.direction;
            if (hit) {
                init_medium = sit.getMedium(wi);
            } else {
                init_medium = medium;
            }
            Spectrum lightSpec =
                directLighting(scene, its, res, pdfLight, medium);
            if (!lightSpec.isZero()) {
                if (hit) {
                    Spectrum f = bsdf->f(wo, wi);
                    L += throughput * f * lightSpec;
                } else {
                    float p = medium->phase.p(wi, wo);
                    L += throughput * p * lightSpec;
                }
            }
        }

        // ruassian roulette
        if (depth > 2 && sampler->next1D() > 0.95f) {
            break;
        }
        throughput /= 0.95f;

        // 下一步光线的方向
        if (hit) {
            auto bsdf_sample_result =
                bsdf->sample(-ray.direction, sampler->next2D());
            if (bsdf_sample_result.weight.isZero()) break;

            throughput *= bsdf_sample_result.weight;
            ray = Ray(sit.position, bsdf_sample_result.wi);
            ray.medium = sit.getMedium(ray.direction);

            specularBounce = bsdf_sample_result.type == BSDFType::Specular;
        } else {
            // scatter
            MediumInScatter mis;
            medium->in_scatter(mit.position, -ray.direction, sampler, mis);
            throughput *= mis.beta;

            ray = Ray(mit.position, mis.wi);
            ray.medium = medium;

            specularBounce = false;
        }
        itsOpt = scene.rayIntersect(ray);
    }

    return L;
}

Spectrum directLighting(const Scene &scene, const Intersection &its,
                        LightSampleResult &res, float sample_pdf,
                        Medium *medium) {
    Ray ray(its.position + res.direction * 1e-4f, res.direction, 1e-4f,
            res.distance);
    ray.medium = medium;
    Vector3f wi = res.direction;

    Spectrum tr = scene.Tr(ray);
    Spectrum spec(0.0f);
    if (!tr.isZero()) {
        res.pdf *= sample_pdf;
        float pdf = convertPDF(res, its);
        spec = tr * res.energy / pdf;
    }
    return spec;
}