// All 2D interface: main menu, server browser, HUD, inventory, crafting,
// storage, death screen, pause menu and chat. Built with Dear ImGui; restyle
// later without touching game code.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

#include "app.h"

namespace sg {

namespace {

float g_scale = 1.0f;
const Renderer* g_renderer = nullptr;
constexpr float kSlot = 58.0f;

ImU32 rgba(float r, float g, float b, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }
const ImVec4 kAccent{0.90f, 0.52f, 0.18f, 1.0f};
const ImVec4 kMuted{0.62f, 0.62f, 0.66f, 1.0f};
const ImVec4 kBad{0.95f, 0.35f, 0.30f, 1.0f};
const ImVec4 kGood{0.45f, 0.85f, 0.45f, 1.0f};

void centerNext(ImVec2 size) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
}

constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
constexpr ImGuiWindowFlags kOverlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

bool wideButton(const char* label, float height = 0) {
  return ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, height > 0 ? height : 44 * g_scale));
}

void textCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 color, const char* text) {
  ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text);
  ImVec2 p = center - ts * 0.5f;
  dl->AddText(font, size, p + ImVec2(1, 1), rgba(0, 0, 0, 0.7f), text);  // drop shadow
  dl->AddText(font, size, p, color, text);
}

void drawItemIcon(ImDrawList* dl, ImVec2 p, float size, const ItemStack& st) {
  const ItemDef& d = itemDef(st.item);
  ImFont* font = ImGui::GetFont();
  const float pad = size * 0.06f;
  unsigned tex = g_renderer ? g_renderer->iconTexture(st.item) : 0;
  if (tex) {
    // 3D icon rendered at startup (GL textures are bottom-up, so flip V).
    dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), p + ImVec2(pad, pad), p + ImVec2(size - pad, size - pad),
                 ImVec2(0, 1), ImVec2(1, 0));
  } else {
    dl->AddRectFilled(p + ImVec2(pad, pad), p + ImVec2(size - pad, size - pad), rgba(d.color.x, d.color.y, d.color.z), 6 * g_scale);
    float fs = ImGui::GetFontSize() * 0.68f;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, d.shortName);
    dl->AddText(font, fs, p + ImVec2((size - ts.x) * 0.5f, (size - ts.y) * 0.45f), rgba(1, 1, 1), d.shortName);
  }
  if (st.count > 1) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", st.count);
    float cs = ImGui::GetFontSize() * 0.72f;
    ImVec2 cz = font->CalcTextSizeA(cs, FLT_MAX, 0, buf);
    ImVec2 cp = p + ImVec2(size - cz.x - 4 * g_scale, size - cz.y - 2 * g_scale);
    dl->AddText(font, cs, cp + ImVec2(1, 1), rgba(0, 0, 0, 0.9f), buf);
    dl->AddText(font, cs, cp, rgba(1, 1, 1), buf);
  }
}

struct SlotPayload {
  uint32_t container;
  int slot;
  int count;  // 0 = whole stack
};

// One inventory/container slot with drag & drop. Right-click quick-moves.
void slotWidget(App& a, uint32_t container, int slot, const ItemStack& st, bool selected) {
  const float size = kSlot * g_scale;
  ImGui::PushID(static_cast<int>(container));
  ImGui::PushID(slot);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("slot", ImVec2(size, size));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, p + ImVec2(size, size), hovered ? rgba(0.28f, 0.28f, 0.30f, 0.95f) : rgba(0.16f, 0.16f, 0.18f, 0.9f), 6 * g_scale);
  if (selected) dl->AddRect(p, p + ImVec2(size, size), ImGui::GetColorU32(kAccent), 6 * g_scale, 0, 2.5f * g_scale);
  if (!st.empty()) drawItemIcon(dl, p, size, st);

  if (!st.empty() && ImGui::BeginDragDropSource()) {
    SlotPayload pl{container, slot, ImGui::GetIO().KeyShift ? std::max(1, st.count / 2) : 0};
    ImGui::SetDragDropPayload("SLOT", &pl, sizeof(pl));
    ImVec2 q = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ItemStack shown = st;
    if (pl.count) shown.count = static_cast<uint16_t>(pl.count);
    drawItemIcon(ImGui::GetWindowDrawList(), q, size, shown);
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SLOT")) {
      const auto* src = static_cast<const SlotPayload*>(pl->Data);
      a.net.moveItem(src->container, src->slot, container, slot, src->count);
    }
    ImGui::EndDragDropTarget();
  }
  if (!st.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    uint32_t target = container != kSelfContainer ? kSelfContainer : a.net.openContainer();
    a.net.moveItem(container, slot, target, kAutoSlot);
  }
  if (hovered && !st.empty() && !ImGui::GetDragDropPayload()) {
    const ItemDef& d = itemDef(st.item);
    ImGui::BeginTooltip();
    ImGui::TextColored(kAccent, "%s", d.name);
    ImGui::TextUnformatted(d.description);
    if (d.kind == ItemKind::Gun) ImGui::TextColored(kMuted, "Damage %.0f  |  Uses %s", d.damage, itemDef(d.ammo).name);
    if (d.kind == ItemKind::Food) ImGui::TextColored(kMuted, "+%.0f food  +%.0f saturation", d.food, d.saturation);
    ImGui::TextColored(kMuted, "Right-click: quick move   Shift-drag: half   Drag outside: drop");
    ImGui::EndTooltip();
  }
  ImGui::PopID();
  ImGui::PopID();
}

void slotGrid(App& a, uint32_t container, const Slots& slots, int first, int count, int columns, int selected = -1) {
  for (int i = 0; i < count; ++i) {
    int s = first + i;
    if (i % columns != 0) ImGui::SameLine(0, 6 * g_scale);
    slotWidget(a, container, s, s < static_cast<int>(slots.size()) ? slots[s] : ItemStack{}, s == selected);
  }
}

bool hasSleepingBag(const App& a) {
  for (const auto& [id, o] : a.net.world().objects())
    if (o.type == ObjType::SleepingBag && o.owner == a.net.playerId()) return true;
  return false;
}

bool nearWorkbench(const App& a) {
  std::vector<const WorldObject*> list;
  a.net.world().near(a.net.predicted().pos, 4.0f, list);
  for (const WorldObject* o : list)
    if (o->type == ObjType::Workbench) return true;
  return false;
}

void bar(ImDrawList* dl, ImVec2 p, ImVec2 size, float frac, ImVec4 color, const char* label, float subFrac = -1,
         ImVec4 subColor = {}) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  dl->AddRectFilled(p, p + size, rgba(0.05f, 0.05f, 0.06f, 0.75f), 4 * g_scale);
  dl->AddRectFilled(p, p + ImVec2(size.x * frac, size.y), ImGui::GetColorU32(color), 4 * g_scale);
  if (subFrac >= 0) {  // thin second bar along the bottom (saturation)
    float h = 4 * g_scale;
    dl->AddRectFilled(p + ImVec2(0, size.y - h), p + ImVec2(size.x * std::clamp(subFrac, 0.0f, 1.0f), size.y),
                      ImGui::GetColorU32(subColor), 2 * g_scale);
  }
  ImGui::PushFont(nullptr);
  ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(p + ImVec2(8 * g_scale, (size.y - ts.y) * 0.5f) + ImVec2(1, 1), rgba(0, 0, 0, 0.8f), label);
  dl->AddText(p + ImVec2(8 * g_scale, (size.y - ts.y) * 0.5f), rgba(1, 1, 1), label);
  ImGui::PopFont();
}

}  // namespace

// --- setup ------------------------------------------------------------------------------
void uiSetup(App& a, float contentScale) {
  g_scale = contentScale > 0.5f ? contentScale : 1.0f;
  g_renderer = &a.renderer;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  const char* regular[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"};
  const char* bold[] = {"C:/Windows/Fonts/segoeuib.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
                        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf"};
  std::error_code ec;
  for (const char* f : regular)
    if (!a.fontBody && std::filesystem::exists(f, ec)) a.fontBody = io.Fonts->AddFontFromFileTTF(f, 18 * g_scale);
  for (const char* f : bold)
    if (!a.fontBig && std::filesystem::exists(f, ec)) {
      a.fontBig = io.Fonts->AddFontFromFileTTF(f, 30 * g_scale);
      a.fontHuge = io.Fonts->AddFontFromFileTTF(f, 72 * g_scale);
    }
  if (!a.fontBody) {
    ImFontConfig cfg;
    cfg.SizePixels = 15 * g_scale;
    a.fontBody = io.Fonts->AddFontDefault(&cfg);
  }
  if (!a.fontBig) a.fontBig = a.fontHuge = a.fontBody;
  io.FontDefault = a.fontBody;

  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10;
  s.FrameRounding = 6;
  s.GrabRounding = 6;
  s.PopupRounding = 6;
  s.ChildRounding = 8;
  s.WindowPadding = ImVec2(18, 16);
  s.FramePadding = ImVec2(12, 8);
  s.ItemSpacing = ImVec2(10, 8);
  s.WindowBorderSize = 0;
  s.ScaleAllSizes(g_scale);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.94f);
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.97f);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.48f, 0.16f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.38f, 0.12f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.24f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.85f, 0.48f, 0.16f, 0.35f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.48f, 0.16f, 0.55f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.48f, 0.16f, 0.75f);
  c[ImGuiCol_SliderGrab] = c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_PlotHistogram] = kAccent;
  c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
}

// --- main menu --------------------------------------------------------------------------
void uiMainMenu(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, rgba(0, 0, 0, 0.55f), rgba(0, 0, 0, 0.1f),
                              rgba(0, 0, 0, 0.1f), rgba(0, 0, 0, 0.55f));
  textCentered(bg, a.fontHuge, a.fontHuge->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f),
               rgba(0.95f, 0.93f, 0.88f), "SURVIVAL");
  textCentered(bg, a.fontBody, a.fontBody->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f + 52 * g_scale),
               ImGui::GetColorU32(kMuted), "Gather. Craft. Build. Survive.");

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.58f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(380 * g_scale, 0), ImGuiCond_Always);
  ImGui::Begin("##menu", nullptr, kPanel | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextColored(kMuted, "Your name");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##name", a.playerName, sizeof(a.playerName));
  ImGui::Spacing();
  if (wideButton("Play")) {
    a.screen = Screen::ServerBrowser;
    a.connectError.clear();
    a.lastPing = -1e9;  // ping right away
  }
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Mouse sensitivity");
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderFloat("##sens", &a.mouseSensitivity, 0.2f, 3.0f, "%.2f");
  ImGui::Spacing();
  if (wideButton("Quit")) a.quit = true;
  ImGui::End();

  textCentered(bg, a.fontBody, a.fontBody->FontSize * 0.85f, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24 * g_scale),
               rgba(1, 1, 1, 0.45f), "Alpha build 0.2  -  custom C++ engine");
}

// --- server browser --------------------------------------------------------------------
void uiServerBrowser(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, rgba(0, 0, 0, 0.45f));
  centerNext(ImVec2(900 * g_scale, 720 * g_scale));
  ImGui::Begin("Servers", nullptr, kPanel);

  ImGui::PushFont(a.fontBig);
  ImGui::TextUnformatted("Play online");
  ImGui::PopFont();
  ImGui::TextColored(kMuted, "Pick a server. Your base, items and progress are saved on each server.");
  ImGui::Spacing();

  int joinIndex = -1;
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable("servers", 4, tf, ImVec2(0, 230 * g_scale))) {
    ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 100 * g_scale);
    ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 80 * g_scale);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 200 * g_scale);
    ImGui::TableHeadersRow();
    for (int i = 0; i < static_cast<int>(a.servers.size()); ++i) {
      const ServerEntry& s = a.servers[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 46 * g_scale);
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      if (ImGui::Selectable("##row", a.selectedServer == i,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0, 40 * g_scale))) {
        a.selectedServer = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) joinIndex = i;
      }
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(s.name.c_str());
      ImGui::TextColored(kMuted, "%s:%u  -  Balanced (PvE coast, PvP inland)", s.host.c_str(), s.port);
      ImGui::EndGroup();
      ImGui::PopID();
      ImGui::TableNextColumn();
      if (s.online) ImGui::Text("%d / %d", s.players, s.maxPlayers);
      else ImGui::TextColored(kMuted, "-");
      ImGui::TableNextColumn();
      if (s.online) ImGui::TextColored(s.pingMs < 80 ? kGood : (s.pingMs < 150 ? kAccent : kBad), "%.0f ms", s.pingMs);
      else ImGui::TextColored(kMuted, "-");
      ImGui::TableNextColumn();
      if (s.online && s.isLocal()) ImGui::TextColored(kGood, "Online - hosted on this PC");
      else if (s.online) ImGui::TextColored(kGood, "Online");
      else if (s.isLocal()) ImGui::TextColored(kAccent, "Starting...");
      else ImGui::TextColored(kBad, "Offline");
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Direct connect (ip or ip:port)");
  ImGui::SetNextItemWidth(320 * g_scale);
  bool enter = ImGui::InputText("##direct", a.directConnect, sizeof(a.directConnect), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if ((ImGui::Button("Connect") || enter) && a.directConnect[0]) {
    ServerEntry e;
    std::string hp = a.directConnect;
    auto colon = hp.find(':');
    e.host = hp.substr(0, colon);
    if (colon != std::string::npos) e.port = static_cast<uint16_t>(std::atoi(hp.substr(colon + 1).c_str()));
    e.name = hp;
    e.online = true;  // never auto-host a typed address
    joinServer(a, e);
  }

  ImGui::SameLine();
  if (ImGui::Button("Add to list") && a.directConnect[0]) ImGui::OpenPopup("Add server");
  bool canRemove = a.selectedServer >= 0 && a.selectedServer < static_cast<int>(a.servers.size()) &&
                   !a.servers[static_cast<size_t>(a.selectedServer)].isLocal();
  ImGui::SameLine();
  ImGui::BeginDisabled(!canRemove);
  if (ImGui::Button("Remove selected")) removeServer(a, a.selectedServer);
  ImGui::EndDisabled();
  if (ImGui::BeginPopupModal("Add server", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Address: %s", a.directConnect);
    ImGui::TextColored(kMuted, "Name for the list");
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::InputText("##addname", a.addName, sizeof(a.addName));
    if (ImGui::Button("Add", ImVec2(140 * g_scale, 0))) {
      addServer(a, a.addName, a.directConnect);
      a.addName[0] = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(140 * g_scale, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (!a.connectError.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(kBad, "%s", a.connectError.c_str());
  }

  // How friends join the worlds hosted on this PC.
  if (!a.hosted.empty()) {
    ImGui::Spacing();
    ImGui::SeparatorText("Invite friends to your worlds");
    std::string ports;
    for (const auto& h : a.hosted) ports += (ports.empty() ? "" : " / ") + std::to_string(h->server->config().port);
    auto copyLine = [&](const char* label, const std::string& addr, ImVec4 color) {
      ImGui::TextColored(kMuted, "%s", label);
      ImGui::SameLine();
      ImGui::TextColored(color, "%s", addr.c_str());
      ImGui::SameLine();
      ImGui::PushID(label);
      if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(addr.c_str());
      ImGui::PopID();
    };
    if (!a.lanAddresses.empty())
      copyLine("Same Wi-Fi / home network:", a.lanAddresses[0] + ":" + std::to_string(a.hosted[0]->server->config().port), kGood);
    PortForwarder::Status fw = a.forwarder.status();
    if (fw.state == PortForwarder::State::Open && !fw.publicIp.empty()) {
      copyLine("Over the internet:", fw.publicIp + ":" + std::to_string(a.hosted[0]->server->config().port), kGood);
      ImGui::TextColored(kMuted, "Your friend pastes this into Direct connect (port %s for the second world). %s",
                         ports.c_str(), fw.message.c_str());
    } else if (fw.state == PortForwarder::State::Working) {
      ImGui::TextColored(kAccent, "%s", fw.message.c_str());
    } else {
      ImGui::TextColored(kAccent, "Internet: %s", fw.message.c_str());
      ImGui::TextWrapped(
          "Option A: in your router settings, forward UDP ports %s to this PC, then give friends your public IP "
          "(search \"what is my ip\"). Option B (easiest): install Tailscale (free) on both PCs, join the same "
          "network, and your friend connects to your Tailscale IP.",
          ports.c_str());
    }
  }

  // Bottom buttons.
  float bw = 160 * g_scale;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 64 * g_scale);
  if (ImGui::Button("Back", ImVec2(bw, 44 * g_scale))) a.screen = Screen::MainMenu;
  ImGui::SameLine();
  if (ImGui::Button("Refresh", ImVec2(bw, 44 * g_scale))) a.lastPing = -1e9;
  ImGui::SameLine(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
  bool canJoin = a.selectedServer >= 0 && a.selectedServer < static_cast<int>(a.servers.size());
  const char* joinLabel = canJoin && !a.servers[a.selectedServer].online && a.servers[a.selectedServer].isLocal()
                              ? "Host & Join"
                              : "Join";
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.40f, 0.12f, 1.0f));
  if (ImGui::Button(joinLabel, ImVec2(bw, 44 * g_scale)) && canJoin) joinIndex = a.selectedServer;
  ImGui::PopStyleColor();
  ImGui::End();

  if (joinIndex >= 0) joinServer(a, a.servers[joinIndex]);
}

// --- connecting -----------------------------------------------------------------------------
void uiConnecting(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, rgba(0, 0, 0, 0.55f));
  centerNext(ImVec2(440 * g_scale, 0));
  ImGui::Begin("##connecting", nullptr, kPanel | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PushFont(a.fontBig);
  ImGui::TextUnformatted("Connecting");
  ImGui::PopFont();
  int dots = static_cast<int>(a.now * 3) % 4;
  ImGui::Text("Joining %s%.*s", a.connectingTo.c_str(), dots, "...");
  ImGui::ProgressBar(static_cast<float>(std::fmod(a.now * 0.7, 1.0)), ImVec2(-1, 6 * g_scale), "");
  ImGui::Spacing();
  if (wideButton("Cancel")) leaveServer(a);
  ImGui::End();
}

// --- in game --------------------------------------------------------------------------------
static void hudCrosshair(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 c = io.DisplaySize * 0.5f;
  dl->AddCircleFilled(c, 2.5f * g_scale, rgba(1, 1, 1, 0.9f));
  dl->AddCircle(c, 3.5f * g_scale, rgba(0, 0, 0, 0.5f));
  double since = a.now - a.net.lastHitTime();
  if (since < 0.25) {  // hit marker
    uint8_t f = a.net.lastHitFlags();
    ImU32 col = (f & 2) ? rgba(1, 0.2f, 0.2f) : (f & 1) ? rgba(1, 0.85f, 0.2f) : rgba(1, 1, 1);
    float r1 = 6 * g_scale, r2 = 13 * g_scale;
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2)
        dl->AddLine(c + ImVec2(sx * r1, sy * r1), c + ImVec2(sx * r2, sy * r2), col, 2.5f * g_scale);
  }
}

static void hudVitals(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Vitals& v = a.net.vitals();
  const PlayerState& s = a.net.predicted();
  ImVec2 size(240 * g_scale, 26 * g_scale);
  float gap = 6 * g_scale;
  ImVec2 p(io.DisplaySize.x - size.x - 24 * g_scale, io.DisplaySize.y - 3 * (size.y + gap) - 24 * g_scale);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Health  %.0f", std::ceil(v.health));
  bar(dl, p, size, v.health / kMaxHealth, ImVec4(0.75f, 0.18f, 0.16f, 0.95f), buf);
  p.y += size.y + gap;
  std::snprintf(buf, sizeof(buf), "Food  %.0f   +%.0f", v.food, v.saturation);
  bar(dl, p, size, v.food / kMaxFood, v.food < 20 ? ImVec4(0.85f, 0.30f, 0.10f, 0.95f) : ImVec4(0.72f, 0.48f, 0.15f, 0.95f),
      buf, v.saturation / kMaxSaturation, ImVec4(1.0f, 0.86f, 0.35f, 1.0f));
  p.y += size.y + gap;
  std::snprintf(buf, sizeof(buf), s.exhausted ? "Stamina  %.0f  (exhausted)" : "Stamina  %.0f", s.stamina);
  bar(dl, p, size, s.stamina / kMaxStamina, s.exhausted ? ImVec4(0.55f, 0.55f, 0.20f, 0.95f) : ImVec4(0.25f, 0.62f, 0.32f, 0.95f), buf);
  if (v.food <= 0) {
    textCentered(dl, a.fontBody, a.fontBody->FontSize, ImVec2(p.x + size.x * 0.5f, p.y - 3 * (size.y + gap) - 14 * g_scale),
                 rgba(1, 0.4f, 0.3f), "Starving! Eat something");
  }
}

static void hudHotbar(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float size = 64 * g_scale, gap = 6 * g_scale;
  const float total = kHotbarSlots * size + (kHotbarSlots - 1) * gap;
  ImVec2 p((io.DisplaySize.x - total) * 0.5f, io.DisplaySize.y - size - 22 * g_scale);
  const Slots& inv = a.net.inventory();
  for (int i = 0; i < kHotbarSlots; ++i) {
    ImVec2 q = p + ImVec2(i * (size + gap), 0);
    dl->AddRectFilled(q, q + ImVec2(size, size), rgba(0.08f, 0.08f, 0.09f, 0.7f), 6 * g_scale);
    if (!inv[i].empty()) drawItemIcon(dl, q, size, inv[i]);
    if (i == a.hotbar) dl->AddRect(q, q + ImVec2(size, size), ImGui::GetColorU32(kAccent), 6 * g_scale, 0, 3 * g_scale);
    char num[4];
    std::snprintf(num, sizeof(num), "%d", i + 1);
    dl->AddText(q + ImVec2(5 * g_scale, 2 * g_scale), rgba(1, 1, 1, 0.6f), num);
  }
  // Held item name (+ ammo) above the hotbar.
  Item held = a.heldItem();
  if (held != Item::None) {
    const ItemDef& d = itemDef(held);
    char buf[96];
    if (d.kind == ItemKind::Gun)
      std::snprintf(buf, sizeof(buf), "%s   -   %d %s", d.name, countItem(inv, d.ammo), itemDef(d.ammo).name);
    else
      std::snprintf(buf, sizeof(buf), "%s", d.name);
    textCentered(dl, a.fontBody, a.fontBody->FontSize, ImVec2(io.DisplaySize.x * 0.5f, p.y - 16 * g_scale), rgba(1, 1, 1, 0.9f), buf);
  }
}

static void hudMessages(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // Kill feed / chat / info, top-left.
  float y = 18 * g_scale;
  const auto& notices = a.net.notices();
  int shown = 0;
  const std::string* latestWarning = nullptr;
  double warnTime = -1e9;
  for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
    if (it->kind == TextKind::Warning) {
      if (!latestWarning && a.now - it->time < 2.5) {
        latestWarning = &it->text;
        warnTime = it->time;
      }
      continue;
    }
    double age = a.now - it->time;
    if (age > 12 && !a.chatOpen) continue;
    if (++shown > 10) break;
  }
  y += (std::min(shown, 10) - 1) * 24 * g_scale;
  shown = 0;
  for (auto it = notices.rbegin(); it != notices.rend() && shown < 10; ++it) {
    if (it->kind == TextKind::Warning) continue;
    double age = a.now - it->time;
    if (age > 12 && !a.chatOpen) continue;
    float alpha = a.chatOpen ? 1.0f : static_cast<float>(std::clamp(12.0 - age, 0.0, 1.0));
    ImU32 col = it->kind == TextKind::Chat       ? rgba(0.65f, 0.85f, 1.0f, alpha)
                : it->kind == TextKind::Kill     ? rgba(1.0f, 0.55f, 0.45f, alpha)
                : it->kind == TextKind::Whisper  ? rgba(0.95f, 0.6f, 1.0f, alpha)
                : it->kind == TextKind::Announce ? rgba(1.0f, 0.85f, 0.3f, alpha)
                                                 : rgba(0.92f, 0.92f, 0.92f, alpha);
    // Staff names stand out in chat.
    if (it->kind == TextKind::Chat && it->text.rfind("[Owner]", 0) == 0) col = rgba(1.0f, 0.72f, 0.25f, alpha);
    if (it->kind == TextKind::Chat && it->text.rfind("[Admin]", 0) == 0) col = rgba(1.0f, 0.45f, 0.40f, alpha);
    if (it->kind == TextKind::Chat && it->text.rfind("[Moderator]", 0) == 0) col = rgba(0.45f, 0.95f, 0.55f, alpha);
    ImVec2 pos(20 * g_scale, y);
    dl->AddText(pos + ImVec2(1, 1), rgba(0, 0, 0, 0.8f * alpha), it->text.c_str());
    dl->AddText(pos, col, it->text.c_str());
    y -= 24 * g_scale;
    ++shown;
  }
  // Announcements: big, centred, for 6 seconds.
  for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
    if (it->kind != TextKind::Announce) continue;
    double age = a.now - it->time;
    if (age < 6) {
      float alpha = static_cast<float>(std::clamp(6.0 - age, 0.0, 1.0));
      textCentered(dl, a.fontBig, a.fontBig->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.22f),
                   rgba(1.0f, 0.88f, 0.4f, alpha), it->text.c_str());
    }
    break;
  }
  // Latest warning in the middle, above the hotbar ("Not enough wood", ...).
  if (latestWarning) {
    float alpha = static_cast<float>(std::clamp(2.5 - (a.now - warnTime), 0.0, 1.0));
    textCentered(dl, a.fontBody, a.fontBody->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 150 * g_scale),
                 rgba(1.0f, 0.82f, 0.35f, alpha), latestWarning->c_str());
  }
  // Gathered resources, bottom-left.
  float gy = io.DisplaySize.y - 60 * g_scale;
  for (auto it = a.net.gathered().rbegin(); it != a.net.gathered().rend(); ++it) {
    double age = a.now - it->time;
    if (age > 3) continue;
    float alpha = static_cast<float>(std::clamp(3.0 - age, 0.0, 1.0));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "+%d %s", it->count, itemDef(it->item).name);
    ImVec2 pos(24 * g_scale, gy);
    dl->AddText(pos + ImVec2(1, 1), rgba(0, 0, 0, 0.8f * alpha), buf);
    dl->AddText(pos, rgba(0.6f, 1.0f, 0.6f, alpha), buf);
    gy -= 24 * g_scale;
  }
  // Interaction prompt under the crosshair.
  if (!a.prompt.empty())
    textCentered(dl, a.fontBody, a.fontBody->FontSize, io.DisplaySize * 0.5f + ImVec2(0, 48 * g_scale), rgba(1, 1, 1, 0.95f),
                 a.prompt.c_str());
}

static void hudNametags(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const Vec3 me = a.net.predicted().pos;
  for (const auto& p : a.net.remotePlayers(a.now)) {
    if (length(p.pos - me) > 40.0f || (p.flags & kEntityDead)) continue;
    float sx, sy;
    if (!a.renderer.project(p.pos + Vec3{0, 2.15f, 0}, sx, sy)) continue;
    std::string name = a.net.nameOf(p.id);
    Rank rank = a.net.rankOf(p.id);
    ImU32 col = rank == Rank::Owner ? rgba(1.0f, 0.72f, 0.25f, 0.95f)
                : rank == Rank::Admin ? rgba(1.0f, 0.45f, 0.40f, 0.95f)
                : rank == Rank::Moderator ? rgba(0.45f, 0.95f, 0.55f, 0.95f)
                                          : rgba(1, 1, 1, 0.85f);
    if (rank != Rank::Player) name = "[" + std::string(rankName(rank)) + "] " + name;
    textCentered(dl, a.fontBody, a.fontBody->FontSize * 0.9f, ImVec2(sx * io.DisplaySize.x, sy * io.DisplaySize.y), col,
                 name.c_str());
  }
}

static void hudInfo(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  char buf[160];
  const Vec3 p = a.net.predicted().pos;
  std::snprintf(buf, sizeof(buf), "%s  |  %.0f fps  |  %.0f ms", a.net.serverName().c_str(), a.fps, a.net.pingMs());
  ImVec2 ts = ImGui::CalcTextSize(buf);
  dl->AddText(ImVec2(io.DisplaySize.x - ts.x - 16 * g_scale, 12 * g_scale), rgba(1, 1, 1, 0.5f), buf);
  if (!a.inventoryOpen)
    dl->AddText(ImVec2(io.DisplaySize.x - 330 * g_scale, io.DisplaySize.y - 140 * g_scale - 3 * 32 * g_scale),
                rgba(1, 1, 1, 0.45f), "Tab inventory   E use   R piece   P players   Enter chat (/help)");

  // Compass strip with the clock under it.
  const float cx = io.DisplaySize.x * 0.5f, top = 10 * g_scale, width = 420 * g_scale;
  dl->AddRectFilled(ImVec2(cx - width * 0.5f, top), ImVec2(cx + width * 0.5f, top + 26 * g_scale), rgba(0, 0, 0, 0.3f), 6 * g_scale);
  // Heading in degrees, 0 = north (-Z), clockwise.
  float heading = std::fmod(-a.yaw * 180.0f / kPi + 3600.0f, 360.0f);
  const char* labels[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  for (int deg = 0; deg < 360; deg += 15) {
    float d = std::fmod(static_cast<float>(deg) - heading + 540.0f, 360.0f) - 180.0f;  // -180..180
    if (std::fabs(d) > 60) continue;
    float x = cx + d / 60.0f * width * 0.5f;
    float fade = 1.0f - std::fabs(d) / 60.0f;
    if (deg % 45 == 0) {
      textCentered(dl, a.fontBody, a.fontBody->FontSize * (deg % 90 == 0 ? 1.0f : 0.8f), ImVec2(x, top + 13 * g_scale),
                   deg == 0 ? rgba(1.0f, 0.5f, 0.3f, fade) : rgba(1, 1, 1, fade), labels[deg / 45]);
    } else {
      dl->AddLine(ImVec2(x, top + 9 * g_scale), ImVec2(x, top + 17 * g_scale), rgba(1, 1, 1, 0.5f * fade), 1.5f);
    }
  }
  dl->AddTriangleFilled(ImVec2(cx - 5 * g_scale, top + 26 * g_scale), ImVec2(cx + 5 * g_scale, top + 26 * g_scale),
                        ImVec2(cx, top + 20 * g_scale), ImGui::GetColorU32(kAccent));
  float tod = a.net.timeOfDay();
  char clockBuf[32];
  std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d  %s", static_cast<int>(tod) % 24,
                static_cast<int>((tod - std::floor(tod)) * 60), (tod >= 6 && tod < 20) ? "Day" : "Night");
  textCentered(dl, a.fontBody, a.fontBody->FontSize * 0.85f, ImVec2(cx, top + 40 * g_scale), rgba(1, 1, 1, 0.7f), clockBuf);
}

// Who's online, with rank and (for staff) quick admin buttons that run the chat commands.
static void playerList(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640 * g_scale, 460 * g_scale), ImGuiCond_Appearing);
  if (!ImGui::Begin("Players", &a.showPlayers, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  const Rank me = a.net.myRank();
  ImGui::TextColored(kMuted, "You are %s. Ranks: Owner > Admin > Moderator > Player.", rankName(me));
  std::vector<std::pair<uint32_t, NetClient::PlayerEntry>> list(a.net.players().begin(), a.net.players().end());
  std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
    return x.second.rank != y.second.rank ? x.second.rank > y.second.rank : x.second.name < y.second.name;
  });
  if (ImGui::BeginTable("plist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 110 * g_scale);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 300 * g_scale);
    ImGui::TableHeadersRow();
    for (const auto& [id, p] : list) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.name.c_str());
      if (id == a.net.playerId()) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "(you)");
      }
      ImGui::TableNextColumn();
      ImVec4 rc = p.rank == Rank::Owner ? ImVec4(1.0f, 0.72f, 0.25f, 1) : p.rank == Rank::Admin ? ImVec4(1.0f, 0.45f, 0.4f, 1)
                  : p.rank == Rank::Moderator ? ImVec4(0.45f, 0.95f, 0.55f, 1) : kMuted;
      ImGui::TextColored(rc, "%s", rankName(p.rank));
      ImGui::TableNextColumn();
      if (id == a.net.playerId() || me <= p.rank) continue;  // can only act on lower ranks
      ImGui::PushID(static_cast<int>(id));
      auto cmd = [&](const char* label, const std::string& c) {
        if (ImGui::SmallButton(label)) a.net.chat(c);
        ImGui::SameLine();
      };
      if (me >= Rank::Moderator) {
        cmd("TP", "/tp " + p.name);
        cmd("Kick", "/kick " + p.name);
        cmd("Mute", "/mute " + p.name);
      }
      if (me >= Rank::Admin) {
        cmd("Ban", "/ban " + p.name);
        if (ImGui::SmallButton("Rank...")) ImGui::OpenPopup("rankpop");
        if (ImGui::BeginPopup("rankpop")) {
          for (Rank r : {Rank::Player, Rank::Moderator, Rank::Admin, Rank::Owner}) {
            bool allowed = r < me || (me == Rank::Owner && r == Rank::Owner);
            ImGui::BeginDisabled(!allowed);
            if (ImGui::MenuItem(rankName(r))) a.net.chat("/rank " + p.name + " " + rankName(r));
            ImGui::EndDisabled();
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

static void inventoryScreen(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  const Slots& inv = a.net.inventory();
  const float slot = kSlot * g_scale, gap = 6 * g_scale;
  const float gridW = 6 * slot + 5 * gap + 2 * ImGui::GetStyle().WindowPadding.x;

  // Full-screen drop zone behind the windows: dropping an item here throws it on the ground.
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##dropzone", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, rgba(0, 0, 0, 0.35f));
  ImGui::InvisibleButton("##drop", io.DisplaySize);
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SLOT")) {
      const auto* src = static_cast<const SlotPayload*>(pl->Data);
      a.net.dropItem(src->container, src->slot);
    }
    ImGui::EndDragDropTarget();
  }
  ImGui::End();

  // Inventory (centre).
  const float invH = 6 * slot + 5 * gap + 120 * g_scale;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - gridW * 0.5f, io.DisplaySize.y * 0.5f - invH * 0.5f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(gridW, invH), ImGuiCond_Always);
  ImGui::Begin("Inventory", nullptr, kPanel);
  slotGrid(a, kSelfContainer, inv, kHotbarSlots, kInventorySlots - kHotbarSlots, 6);
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Hotbar");
  slotGrid(a, kSelfContainer, inv, 0, kHotbarSlots, 6, a.hotbar);
  ImGui::End();

  // Crafting (right).
  const float craftW = 430 * g_scale;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f + gridW * 0.5f + 16 * g_scale, io.DisplaySize.y * 0.5f - invH * 0.5f),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(craftW, invH), ImGuiCond_Always);
  ImGui::Begin("Crafting", nullptr, kPanel);
  const bool bench = nearWorkbench(a);
  ImGui::BeginChild("recipes", ImVec2(0, invH - 230 * g_scale));
  const auto& list = recipes();
  for (int i = 0; i < static_cast<int>(list.size()); ++i) {
    const Recipe& r = list[i];
    const ItemDef& out = itemDef(r.output);
    bool afford = canAfford(inv, r, 1);
    bool usable = afford && (!r.workbench || bench);
    ImGui::PushID(i);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float icon = 40 * g_scale;
    ImGui::Dummy(ImVec2(icon, icon));
    drawItemIcon(ImGui::GetWindowDrawList(), p, icon, {r.output, r.outCount});
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (r.outCount > 1) ImGui::TextColored(usable ? ImVec4(1, 1, 1, 1) : kMuted, "%s x%u", out.name, r.outCount);
    else ImGui::TextColored(usable ? ImVec4(1, 1, 1, 1) : kMuted, "%s", out.name);
    std::string cost;
    for (const auto& in : r.in) {
      if (in.item == Item::None) continue;
      if (!cost.empty()) cost += ", ";
      cost += std::to_string(in.count) + " " + itemDef(in.item).name;
    }
    ImGui::TextColored(afford ? kMuted : kBad, "%s", cost.c_str());
    if (r.workbench) {
      ImGui::SameLine();
      ImGui::TextColored(bench ? kGood : kAccent, bench ? "[bench]" : "[needs workbench]");
    }
    ImGui::EndGroup();
    ImGui::EndGroup();
    ImGui::SameLine(craftW - 140 * g_scale);
    ImGui::BeginDisabled(!usable);
    if (ImGui::Button("Craft", ImVec2(70 * g_scale, 0))) a.net.craft(i, 1);
    ImGui::SameLine();
    if (ImGui::Button("x5", ImVec2(40 * g_scale, 0))) a.net.craft(i, 5);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", out.description);
    ImGui::Separator();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Crafting queue");
  const auto& q = a.net.craftQueue();
  if (q.empty()) ImGui::TextColored(kMuted, "Nothing being crafted");
  for (int i = 0; i < static_cast<int>(q.size()) && i < 4; ++i) {
    const auto& job = q[i];
    float frac = 0;
    if (i == 0) {
      double rem = std::max(0.0, job.remaining - (a.now - a.net.craftQueueTime()));
      frac = job.total > 0 ? 1.0f - static_cast<float>(rem / job.total) : 1.0f;
    }
    ImGui::PushID(1000 + i);
    ImGui::ProgressBar(frac, ImVec2(craftW - 110 * g_scale, 22 * g_scale), itemDef(recipes()[job.recipe].output).name);
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel")) a.net.cancelCraft(i);
    ImGui::PopID();
  }
  if (q.size() > 4) ImGui::TextColored(kMuted, "+%zu more", q.size() - 4);
  ImGui::End();

  // Open container (left).
  if (a.net.openContainer()) {
    const Slots& cs = a.net.containerSlots();
    int cols = 6;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - gridW * 1.5f - 16 * g_scale, io.DisplaySize.y * 0.5f - invH * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(gridW, invH), ImGuiCond_Always);
    ImGui::Begin(a.net.openContainerType() == ObjType::LootBag ? "Loot Bag" : "Storage Box", nullptr, kPanel);
    slotGrid(a, a.net.openContainer(), cs, 0, static_cast<int>(cs.size()), cols);
    ImGui::Spacing();
    if (wideButton("Take all", 36 * g_scale))
      for (int i = 0; i < static_cast<int>(cs.size()); ++i)
        if (!cs[i].empty()) a.net.moveItem(a.net.openContainer(), i, kSelfContainer, kAutoSlot);
    ImGui::End();
  }
}

static void deathScreen(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, rgba(0.25f, 0.0f, 0.0f, 0.55f));
  textCentered(bg, a.fontHuge, a.fontHuge->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f),
               rgba(0.95f, 0.25f, 0.2f), "YOU DIED");
  const std::string& cause = a.net.deathCause();
  textCentered(bg, a.fontBody, a.fontBody->FontSize, ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f + 56 * g_scale),
               rgba(1, 1, 1, 0.85f), cause.empty() ? "You died" : cause.c_str());
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.55f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(360 * g_scale, 0), ImGuiCond_Always);
  ImGui::Begin("##death", nullptr, kPanel | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextColored(kMuted, "Your items are in a loot bag where you died.");
  ImGui::TextColored(kMuted, "Everything in your boxes is safe.");
  ImGui::Spacing();
  bool bag = hasSleepingBag(a);
  ImGui::BeginDisabled(!bag);
  if (wideButton(bag ? "Respawn at sleeping bag" : "No sleeping bag placed")) a.net.respawn(true);
  ImGui::EndDisabled();
  if (wideButton("Respawn on a random beach")) a.net.respawn(false);
  ImGui::End();
}

static void pauseMenu(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, rgba(0, 0, 0, 0.5f));
  centerNext(ImVec2(420 * g_scale, 0));
  ImGui::Begin("Paused", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize);
  if (wideButton("Resume")) a.pauseOpen = false;
  if (wideButton("Players and ranks (P)")) a.showPlayers = true;
  ImGui::Spacing();
  ImGui::TextColored(kMuted, "Mouse sensitivity");
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderFloat("##sens", &a.mouseSensitivity, 0.2f, 3.0f, "%.2f");
  ImGui::Checkbox("Third-person camera (V)", &a.thirdPerson);
  ImGui::Spacing();
  ImGui::SeparatorText("Controls");
  ImGui::TextColored(kMuted,
                     "WASD move, Shift sprint, Space jump\n"
                     "Left click: hit / gather / shoot / eat / build\n"
                     "1-6 or mouse wheel: hotbar    Tab: inventory & crafting\n"
                     "E: open doors, boxes, pick plants    R: change building piece\n"
                     "Hammer: hit your building to upgrade it\n"
                     "Enter: chat    V: camera    Esc: this menu");
  ImGui::Spacing();
  if (wideButton("Leave server")) leaveServer(a);
  if (wideButton("Quit game")) a.quit = true;
  ImGui::End();
}

static void chatBox(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(16 * g_scale, io.DisplaySize.y * 0.5f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(520 * g_scale, 0), ImGuiCond_Always);
  ImGui::Begin("##chat", nullptr, kPanel | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
  if (a.chatJustOpened) {
    ImGui::SetKeyboardFocusHere();
    a.chatJustOpened = false;
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##chatinput", "Say something, or /help for commands (Enter sends, Esc cancels)", a.chatBuf,
                               sizeof(a.chatBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
    a.net.chat(a.chatBuf);
    a.chatBuf[0] = 0;
    a.chatOpen = false;
  }
  ImGui::End();
}

static void buildHint(App& a) {
  if (!a.havePlacement) return;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Placement& p = a.placement;
  std::string line = objName(p.type);
  if (isBuildingPiece(p.type)) {
    Ingredient cost = buildCost(p.type, Tier::Twig);
    line += "  -  " + std::to_string(cost.count) + " " + itemDef(cost.item).name + " (have " +
            std::to_string(countItem(a.net.inventory(), cost.item)) + ")   R: next piece";
  }
  textCentered(dl, a.fontBody, a.fontBody->FontSize, io.DisplaySize * 0.5f + ImVec2(0, 80 * g_scale), rgba(1, 1, 1, 0.9f),
               line.c_str());
  if (p.error)
    textCentered(dl, a.fontBody, a.fontBody->FontSize, io.DisplaySize * 0.5f + ImVec2(0, 106 * g_scale),
                 rgba(1.0f, 0.45f, 0.35f), p.error);
}

void uiInGame(App& a) {
  hudNametags(a);
  const bool dead = a.net.vitals().dead;
  if (!dead) {
    if (!a.inventoryOpen && !a.pauseOpen) hudCrosshair(a);
    hudVitals(a);
    hudHotbar(a);
    if (!a.inventoryOpen) buildHint(a);
  }
  hudMessages(a);
  hudInfo(a);
  if (dead) deathScreen(a);
  else if (a.inventoryOpen) inventoryScreen(a);
  if (a.chatOpen) chatBox(a);
  if (a.pauseOpen) pauseMenu(a);
  if (a.showPlayers) playerList(a);
}

}  // namespace sg
