#include <windows.h>
#include "nexus/Nexus.h"

static AddonDefinition_t s_AddonDef = {};

static void AddonLoad(AddonAPI_t*) {}
static void AddonUnload() {}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    s_AddonDef.Signature   = 0x504C5457;
    s_AddonDef.APIVersion  = NEXUS_API_VERSION;
    s_AddonDef.Name        = "Plot Twist";
    s_AddonDef.Version     = {0, 1, 0, 0};
    s_AddonDef.Author      = "";
    s_AddonDef.Description = "Browse homestead decorations";
    s_AddonDef.Load        = AddonLoad;
    s_AddonDef.Unload      = AddonUnload;
    s_AddonDef.Flags       = AF_None;
    s_AddonDef.Provider    = UP_GitHub;
    s_AddonDef.UpdateLink  = "";
    return &s_AddonDef;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
