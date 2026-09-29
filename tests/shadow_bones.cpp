#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <cmath>
#include <limits>
#include <windows.h>
#include <d3d9.h>
#define NVR_GPU_PROFILER_TEST
#include "../src/core/GpuProfiler.h"
#include "../src/core/ShadowBoneUpload.h"
#include "../src/core/ShadowFaceCull.h"

void CheckShadowFaces() {
    const float directions[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    unsigned int count = 0;
    for (const auto& d : directions)
        count += ShadowSphereTouchesFace(100, 0, 0, 1, d[0], d[1], d[2]);
    assert(count == 1); // six submissions reduced to one for this small caster
    assert(ShadowSphereTouchesFace(100, 100, 0, 1, 1, 0, 0));
    assert(ShadowSphereTouchesFace(100, 100, 0, 1, 0, 1, 0));
    assert(ShadowSphereTouchesFace(-100, 0, 0, 0, 1, 0, 0));
    assert(ShadowSphereTouchesFace(0, 0, 0, std::numeric_limits<float>::quiet_NaN(), 1, 0, 0));
    std::mt19937 rng(927);
    std::uniform_real_distribution<float> position(-200, 200), unit(-1, 1), radius(0.01f, 100);
    for (unsigned int trial = 0; trial < 100000; ++trial) {
        const float center[3] = {position(rng), position(rng), position(rng)};
        const float r = radius(rng);
        for (const auto& d : directions) {
            const bool accepted = ShadowSphereTouchesFace(center[0], center[1], center[2], r, d[0], d[1], d[2]);
            // Independent point-in-face oracle: every visible sampled point in
            // the sphere requires that its enclosing sphere be accepted.
            for (int sample = 0; sample < 8; ++sample) {
                float v[3] = {unit(rng), unit(rng), unit(rng)};
                if (v[0]*v[0] + v[1]*v[1] + v[2]*v[2] > 1) continue;
                for (int k = 0; k < 3; ++k) v[k] = center[k] + r * v[k];
                const float forward = v[0]*d[0] + v[1]*d[1] + v[2]*d[2];
                const float sideA = d[0] ? v[1] : v[0];
                const float sideB = d[2] ? v[1] : v[2];
                if (forward >= std::abs(sideA) && forward >= std::abs(sideB)) assert(accepted);
            }
        }
    }
    std::puts("PASS: 100,000 randomized shadow spheres, six face directions, seams and invalid bounds.");
    std::puts("Synthetic small +X caster submissions: 6 -> 1 (not an FPS measurement).");
}

struct DeviceSink {
    std::array<float, 256 * 4> registers;
    unsigned int calls = 0;
    DeviceSink() { registers.fill(-1.0f); }
    void SetVertexShaderConstantF(unsigned int first, const float* data, unsigned int count) {
        assert(first + count <= 256);
        std::copy(data, data + count * 4, registers.begin() + first * 4);
        ++calls;
    }
};

unsigned int Check(const unsigned short* map, unsigned int count) {
    std::array<float, 256 * 12> matrices;
    for (unsigned int i = 0; i < matrices.size(); ++i) matrices[i] = static_cast<float>(i);
    DeviceSink reference, batched;
    for (unsigned int i = 0; i < count; ++i) {
        unsigned int index = map ? map[i] : i;
        reference.SetVertexShaderConstantF(9 + i * 3, matrices.data() + index * 12, 3);
    }
    UploadShadowBones(&batched, matrices.data(), map, count);
    // Includes untouched registers before/after the palette, not only bone data.
    assert(reference.registers == batched.registers);
    assert(batched.calls <= reference.calls);
    return batched.calls;
}

void CheckGpuProfiler();
void CheckCompositeMath();
int main() {
	CheckCompositeMath();
	CheckGpuProfiler();
	assert(GpuProfileQueriesReady(S_OK, S_OK, S_OK));
	assert(!GpuProfileQueriesReady(S_FALSE, S_OK, S_OK));
	assert(!GpuProfileQueriesReady(S_OK, S_FALSE, S_OK));
	assert(!GpuProfileQueriesReady(S_OK, S_OK, S_FALSE));
	std::puts("PASS: pending D3D9 query results are never consumed as zero timestamps.");
    CheckShadowFaces();
    assert(Check(nullptr, 0) == 0);
    assert(Check(nullptr, 1) == 1);
    assert(Check(nullptr, 18) == 1);
    assert(Check(nullptr, 82) == 1);
    const unsigned short identity[] = {0, 1, 2, 3, 4};
    const unsigned short offset[] = {100, 101, 102};
    const unsigned short runs[] = {3, 4, 5, 0, 1, 9, 10};
    const unsigned short repeats[] = {3, 3, 3, 3};
    const unsigned short reverse[] = {9, 8, 7, 6};
    assert(Check(identity, 5) == 1);
    assert(Check(offset, 3) == 1);
    assert(Check(runs, 7) == 3);
    assert(Check(repeats, 4) == 4);
    assert(Check(reverse, 4) == 4);
    std::mt19937 random(20260926);
    for (unsigned int trial = 0; trial < 10000; ++trial) {
        unsigned short map[82];
        for (auto& index : map) index = static_cast<unsigned short>(random() % 256);
        Check(map, random() % 83);
    }
    std::puts("PASS: 10,000 randomized palettes and edge cases match original registers exactly.");
    std::puts("API calls: 18 contiguous bones: 18 -> 1; seven bones in three runs: 7 -> 3.");
}
