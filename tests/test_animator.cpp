#include <cstdio>
#include <cmath>
#include "AnimatorController.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a-b) < 1e-4f; }

static AnimatorController MakeLocomotion() {
    AnimatorController c;
    c.name = "Loco";
    c.params = { {"speed", AnimParamType::Float}, {"hit", AnimParamType::Trigger} };
    c.states = { {"Idle","Survey",false}, {"Walk","Walk",true}, {"Run","Run",true}, {"Hit","Hit",false} };
    c.transitions = {
        {"Idle","Walk",0.2f,{{"speed",AnimCondOp::Greater,0.1f}}},
        {"Walk","Run", 0.2f,{{"speed",AnimCondOp::Greater,4.0f}}},
        {"Run","Walk", 0.2f,{{"speed",AnimCondOp::LessEqual,4.0f}}},
        {"Walk","Idle",0.2f,{{"speed",AnimCondOp::LessEqual,0.1f}}},
        {"*","Hit",    0.1f,{{"hit",AnimCondOp::Greater,0.0f}}},   // anyState trigger
    };
    return c;
}

static void T_eval_condition() {
    EXPECT(EvalCondition(AnimCondOp::Greater, 5.0f, 4.0f));
    EXPECT(!EvalCondition(AnimCondOp::Greater, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::LessEqual, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::Less, 3.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::GreaterEqual, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::Equal, 4.0f, 4.0f));
    // Negative cases.
    EXPECT(!EvalCondition(AnimCondOp::Less, 4.0f, 4.0f));
    EXPECT(!EvalCondition(AnimCondOp::LessEqual, 5.0f, 4.0f));
    EXPECT(!EvalCondition(AnimCondOp::Equal, 3.0f, 4.0f));
}

static void T_select_outgoing_first_match() {
    AnimatorController c = MakeLocomotion();
    int idle = FindState(c, "Idle");
    auto p0 = [](const std::string&){ return 0.0f; };
    EXPECT(SelectTransition(c, idle, p0) == -1);
    auto pSlow = [](const std::string& n){ return n=="speed"?1.0f:0.0f; };
    EXPECT(SelectTransition(c, idle, pSlow) == 0);
}

static void T_select_anystate_first() {
    AnimatorController c = MakeLocomotion();
    int walk = FindState(c, "Walk");
    auto p = [](const std::string& n){ return (n=="speed")?9.0f : (n=="hit")?1.0f : 0.0f; };
    int idx = SelectTransition(c, walk, p);
    EXPECT(idx >= 0 && c.transitions[idx].to == "Hit");
}

static void T_phase_math() {
    EXPECT(nearf(WrapPhase01(1.25f), 0.25f));
    EXPECT(nearf(WrapPhase01(-0.25f), 0.75f));
    EXPECT(nearf(PhaseToTime(0.5f, 2.0f), 1.0f));
    EXPECT(nearf(PhaseToTime(0.5f, 0.8f), 0.4f));
    // Boundary cases.
    EXPECT(nearf(WrapPhase01(1.0f), 0.0f));
    EXPECT(nearf(WrapPhase01(0.0f), 0.0f));
    EXPECT(nearf(WrapPhase01(2.5f), 0.5f));
    EXPECT(nearf(WrapPhase01(-1.25f), 0.75f));
}

static void T_select_uninitialized_state() {
    AnimatorController c = MakeLocomotion();
    // currentState == -1 (uninitialized): all params 0 -> nothing fires.
    auto p0 = [](const std::string&){ return 0.0f; };
    EXPECT(SelectTransition(c, -1, p0) == -1);
    // anyState trigger still fires from an uninitialized current state.
    auto pHit = [](const std::string& n){ return n=="hit"?1.0f:0.0f; };
    int idx = SelectTransition(c, -1, pHit);
    EXPECT(idx >= 0 && c.transitions[idx].to == "Hit");
}

static void T_select_empty_conditions() {
    AnimatorController c;
    c.name = "Trivial";
    c.states = { {"A","A",false}, {"B","B",false} };
    c.transitions = { {"A","B",0.2f,{}} }; // empty conditions -> trivially fires
    int a = FindState(c, "A");
    auto p0 = [](const std::string&){ return 0.0f; };
    EXPECT(SelectTransition(c, a, p0) == 0);
}

static void T_json_roundtrip() {
    AnimatorController c = MakeLocomotion();
    nlohmann::json j = c;
    AnimatorController r = j.get<AnimatorController>();
    EXPECT(r.name == c.name);
    EXPECT(r.params.size() == c.params.size());
    EXPECT(r.states.size() == c.states.size() && r.states[1].cyclic == true);
    EXPECT(r.transitions.size() == c.transitions.size());
    EXPECT(r.transitions[4].from == "*" && r.transitions[4].to == "Hit");
    EXPECT(nearf(r.transitions[0].duration, 0.2f));
    EXPECT(r.transitions[0].conditions.size() == 1 && r.transitions[0].conditions[0].op == AnimCondOp::Greater);
    EXPECT(nearf(r.transitions[0].conditions[0].value, 0.1f));
    EXPECT(r.params.size() >= 2 && r.params[1].type == AnimParamType::Trigger);
}

int main() {
    T_eval_condition();
    T_select_outgoing_first_match();
    T_select_anystate_first();
    T_select_uninitialized_state();
    T_select_empty_conditions();
    T_phase_math();
    T_json_roundtrip();
    if (g_Failures) { std::fprintf(stderr, "test_animator: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All animator tests passed.\n");
    return 0;
}
