#pragma once

#include <array>
#include <algorithm>

// Fixed-capacity rolling ring of float samples for live metric graphs. Newest overwrites oldest
// when full. Pure (no ImGui/glm) so it is unit-testable. Data()/Count()/Offset() map directly onto
// ImGui::PlotLines(values, values_count, values_offset) for a correctly-scrolling plot.
template <int N>
class MetricHistory {
    static_assert(N > 0, "MetricHistory capacity must be positive");
public:
    void Push(float v) {
        m_Data[m_Write] = v;
        m_Write = (m_Write + 1) % N;
        if (m_Count < N) ++m_Count;
    }

    int  Count()    const { return m_Count; }
    int  Capacity() const { return N; }
    // values_offset for ImGui::PlotLines: 0 until the ring wraps, then the write cursor (oldest).
    int  Offset()   const { return (m_Count < N) ? 0 : m_Write; }
    const float* Data() const { return m_Data.data(); }

    float Last() const { return m_Count == 0 ? 0.0f : m_Data[(m_Write + N - 1) % N]; }

    float Min() const {
        if (m_Count == 0) return 0.0f;
        float m = m_Data[0];
        for (int i = 1; i < m_Count; ++i) m = std::min(m, m_Data[i]);
        return m;
    }
    float Max() const {
        if (m_Count == 0) return 0.0f;
        float m = m_Data[0];
        for (int i = 1; i < m_Count; ++i) m = std::max(m, m_Data[i]);
        return m;
    }
    float Avg() const {
        if (m_Count == 0) return 0.0f;
        float s = 0.0f;
        for (int i = 0; i < m_Count; ++i) s += m_Data[i];
        return s / static_cast<float>(m_Count);
    }

private:
    std::array<float, N> m_Data{}; // zero-initialized
    int m_Count = 0;
    int m_Write = 0;
};
