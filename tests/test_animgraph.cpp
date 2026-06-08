#include <cstdio>
#include <cmath>
#include "AnimatorController.h"
#include "AnimatorGraphLayout.h"

static int g_Failures = 0;
#define EXPECT(c) do{ if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } }while(0)
static bool nearf(float a, float b) { return std::fabs(a-b) < 1e-4f; }

static AnimatorController Loco() {
    AnimatorController c; c.name = "Loco";
    c.params = { {"speed", AnimParamType::Float} };
    c.states = { {"Idle","Survey",false,true}, {"Walk","Walk",true,true}, {"Run","Run",true,true} };
    c.transitions = {
        {"Idle","Walk",0.2f,{{"speed",AnimCondOp::Greater,0.1f}}},
        {"Walk","Run", 0.2f,{{"speed",AnimCondOp::Greater,4.0f}}},
        {"*","Idle",   0.1f,{}},
    };
    return c;
}

static void T_tojson_roundtrip() {
    AnimatorController c = Loco();
    nlohmann::json j = c;
    AnimatorController r = j.get<AnimatorController>();
    EXPECT(r.name == "Loco");
    EXPECT(r.params.size() == 1 && r.states.size() == 3 && r.transitions.size() == 3);
    EXPECT(r.states[1].cyclic && r.states[0].loop);
    EXPECT(r.transitions[2].from == "*" && r.transitions[2].to == "Idle");
    EXPECT(!j.contains("stateClipIds"));
}

static void T_exit_time_json() {
    AnimatorController c = Loco();
    c.transitions[0].hasExitTime = true; c.transitions[0].exitTime = 0.75f;
    nlohmann::json j = c; AnimatorController r = j.get<AnimatorController>();
    EXPECT(r.transitions[0].hasExitTime == true && nearf(r.transitions[0].exitTime, 0.75f));
    nlohmann::json jt = { {"from","A"}, {"to","B"}, {"duration",0.2} };
    AnimTransition dt = jt.get<AnimTransition>();
    EXPECT(dt.hasExitTime == false && nearf(dt.exitTime, 1.0f));   // defaults when keys absent
}

static void T_rename_rewrites_transitions() {
    AnimatorController c = Loco();
    RenameState(c, "Walk", "Stroll");
    EXPECT(FindState(c, "Stroll") == 1 && FindState(c, "Walk") < 0);
    EXPECT(c.transitions[0].to == "Stroll");
    EXPECT(c.transitions[1].from == "Stroll");
    EXPECT(c.transitions[2].from == "*");
}

static void T_validate() {
    EXPECT(ValidateController(Loco()).empty());
    AnimatorController dup = Loco(); dup.states.push_back({"Idle","X",false,true});
    EXPECT(!ValidateController(dup).empty());
    AnimatorController dangling = Loco(); dangling.transitions.push_back({"Run","Ghost",0.2f,{}});
    EXPECT(!ValidateController(dangling).empty());
    AnimatorController badparam = Loco();
    badparam.transitions[0].conditions[0].paramName = "nope";
    EXPECT(!ValidateController(badparam).empty());
    AnimatorController empty; EXPECT(!ValidateController(empty).empty());
    auto resolver = [](size_t s){ return s != 2; };
    EXPECT(!ValidateController(Loco(), resolver).empty());
    auto allok = [](size_t){ return true; };
    EXPECT(ValidateController(Loco(), allok).empty());
}

static void T_layout_roundtrip() {
    nlohmann::json doc; doc["name"] = "Loco";
    AnimGraphLayout L; L.nodes["Idle"] = {120,80}; L.nodes["__any__"] = {120,240}; L.pan = {5,6}; L.zoom = 1.5f;
    WriteLayout(doc, L);
    AnimGraphLayout R = ReadLayout(doc);
    EXPECT(R.nodes.at("Idle")[0] == 120 && R.nodes.at("Idle")[1] == 80);
    EXPECT(R.nodes.at("__any__")[1] == 240);
    EXPECT(R.pan[0] == 5 && R.zoom == 1.5f);
    AnimatorController c = doc.get<AnimatorController>();
    EXPECT(c.name == "Loco");
}

int main() {
    T_tojson_roundtrip();
    T_exit_time_json();
    T_rename_rewrites_transitions();
    T_validate();
    T_layout_roundtrip();
    if (g_Failures) { std::fprintf(stderr, "test_animgraph: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All anim-graph tests passed.\n");
    return 0;
}
