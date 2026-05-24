#pragma once
#include <string>

// A short-lived status message (toast). Time is injected (caller passes ImGui::GetTime()) so the
// class is pure and unit-testable. Visible() is true from Set() until `durationSec` later.
class TransientStatus {
public:
    void Set(const std::string& text, bool isError, double now, double durationSec = 3.0) {
        m_Text = text; m_IsError = isError; m_Expiry = now + durationSec;
    }
    bool Visible(double now) const { return !m_Text.empty() && now < m_Expiry; }
    const std::string& Text() const { return m_Text; }
    bool IsError() const { return m_IsError; }
private:
    std::string m_Text;
    bool   m_IsError = false;
    double m_Expiry  = 0.0;
};
