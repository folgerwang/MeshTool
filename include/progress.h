#pragma once

#include <cstdint>
#include <atomic>

// Abstract progress callback interface.
// Replaces QProgressBar* throughout the pipeline so core code has no Qt dependency.

struct IProgressCallback
{
    virtual ~IProgressCallback() = default;
    virtual void SetRange(int min_val, int max_val) = 0;
    virtual void SetValue(int value) = 0;
    virtual void Reset() = 0;
};

// Thread-safe progress for use with ImGui.
// Worker threads call SetValue(); the render thread reads GetFraction().
struct AtomicProgress : public IProgressCallback
{
    std::atomic<int> current{0};
    std::atomic<int> min_val{0};
    std::atomic<int> max_val{100};

    void SetRange(int mn, int mx) override { min_val = mn; max_val = mx; }
    void SetValue(int val) override { current = val; }
    void Reset() override { current = 0; }

    float GetFraction() const
    {
        int range = max_val.load() - min_val.load();
        if (range <= 0) return 0.0f;
        return float(current.load() - min_val.load()) / float(range);
    }
};
