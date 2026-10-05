// CPU check of the gamma-2.2 fusion identity; GPU parity still needs an in-game test.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

void CheckCompositeMath() {
    std::mt19937 random(20260928);
    std::uniform_real_distribution<double> unit(0, 1);
    const std::array<double, 3> luma = {0.2126, 0.7152, 0.0722};
    auto power = [](double x, double exponent) { return std::copysign(std::pow(std::abs(x), exponent), x); };
    double largest = 0;
    for (int trial = 0; trial < 100000; ++trial) {
        std::array<double, 3> scene, linear, reference;
        const bool shadows = (trial & 1) != 0;
        const bool aoEnabled = (trial & 2) != 0;
        const bool farPixel = (trial & 4) != 0;
        const double shadow = unit(random), blend = std::min(shadow + 0.5, 1.0);
        double sceneLuma = 0;
        for (int c = 0; c < 3; ++c) {
            // Signed HDR values and zero, including values around fog's sRGB knee.
            scene[c] = trial % 17 == 0 ? 0 : std::pow(10.0, unit(random) * 5 - 3) * (trial % 11 == 0 ? -1 : 1);
            linear[c] = power(scene[c], 2.2);
            sceneLuma += linear[c] * luma[c];
        }
        for (int c = 0; c < 3; ++c) {
            const double sky = power(unit(random), 2.2);
            if (shadows) linear[c] = std::max(0.0, sceneLuma * shadow * sky * (1 - blend) + linear[c] * shadow * blend);
            reference[c] = shadows ? power(linear[c], 1 / 2.2) : scene[c];
        }
        double referenceLuma = 0, fusedLuma = 0;
        for (int c = 0; c < 3; ++c) {
            referenceLuma += power(reference[c], 2.2) * luma[c];
            fusedLuma += linear[c] * luma[c];
        }
        const double ao = trial % 19 == 0 ? 0 : unit(random), threshold = unit(random);
        auto attenuation = [&](double brightness) {
            return aoEnabled && !farPixel ? ao + (1 - ao) * std::clamp((brightness - threshold) * 3, 0.0, 1.0) : 1.0;
        };
        for (int c = 0; c < 3; ++c) {
            const double oldValue = reference[c] * std::pow(attenuation(referenceLuma), 1 / 2.2);
            const double newValue = power(linear[c] * attenuation(fusedLuma), 1 / 2.2);
            const double error = std::abs(oldValue - newValue) / std::max(1.0, std::abs(oldValue));
            largest = std::max(largest, error);
            assert(std::isfinite(newValue) && error < 1e-10);
        }
    }
    std::printf("PASS: 100,000 composite gamma identities (signed HDR, zero, shadow/AO toggles, far pixels); max relative error %.3g.\n", largest);
}
