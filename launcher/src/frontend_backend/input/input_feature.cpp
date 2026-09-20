#include "input_feature.hpp"

#include "../settings/settings_feature.hpp"
#include "../../services/path_service.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <array>
#include <algorithm>
#include <filesystem>

#ifndef XENON_LAUNCHER_RUNTIME_INPUT
#define XENON_LAUNCHER_RUNTIME_INPUT 0
#endif

#if XENON_LAUNCHER_RUNTIME_INPUT
#include "xenon/input/module_api_provider.hpp"
#include "xenon/input/profile.hpp"
#include "xenon/input/sdl_driver.hpp"
#include "xenon/input/system.hpp"
#include "xenon/input/xinput_driver.hpp"
#endif

namespace xenon::launcher::frontend_backend {
namespace {

QString connectionName(int value) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  using xenon::input::ConnectionType;
  switch (static_cast<ConnectionType>(value)) {
    case ConnectionType::Wired: return QStringLiteral("Wired");
    case ConnectionType::Wireless: return QStringLiteral("Wireless");
    case ConnectionType::Virtual: return QStringLiteral("Virtual");
    case ConnectionType::Unknown: break;
  }
#endif
  return QStringLiteral("Unknown");
}

QString subtypeName(int value) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  using xenon::input::DeviceSubtype;
  switch (static_cast<DeviceSubtype>(value)) {
    case DeviceSubtype::Gamepad: return QStringLiteral("Gamepad");
    case DeviceSubtype::Wheel: return QStringLiteral("Wheel");
    case DeviceSubtype::ArcadeStick: return QStringLiteral("Arcade Stick");
    case DeviceSubtype::FlightStick: return QStringLiteral("Flight Stick / HOTAS");
    case DeviceSubtype::DancePad: return QStringLiteral("Dance Pad");
    case DeviceSubtype::Guitar: return QStringLiteral("Guitar");
    case DeviceSubtype::DrumKit: return QStringLiteral("Drum Kit");
    case DeviceSubtype::Unknown: break;
  }
#endif
  return QStringLiteral("Unknown");
}

}  // namespace

class InputFeature::Impl {
 public:
  Impl(InputFeature& owner, SettingsFeature& settings, PathService& paths, bool test_mode)
      : owner(owner), settings(settings), paths(paths), test_mode(test_mode) {}

  InputFeature& owner;
  SettingsFeature& settings;
  PathService& paths;
  bool test_mode{};
  QString last_status{QStringLiteral("Input runtime unavailable")};

#if XENON_LAUNCHER_RUNTIME_INPUT
  std::unique_ptr<xenon::input::InputSystem> system{};
  std::unique_ptr<xenon::input::module_api::Provider> module_api{};
  std::array<xenon::input::GamepadState, xenon::input::kMaxUsers> frontend_previous_states{};
  std::array<bool, xenon::input::kMaxUsers> frontend_has_previous{};

  QString storePath() const {
    return QDir{paths.configuredPath(QStringLiteral("profiles"))}
        .filePath(QStringLiteral("input-profiles-v1.conf"));
  }

  static QString profileIdForUser(const xenon::input::InputSystem& input,
                                  std::uint32_t user) {
    const auto binding = input.profiles().user_binding(user);
    return binding ? QString::fromStdString(*binding) : QString{};
  }

  std::optional<xenon::input::DeviceId> idForIdentity(const QString& identity) const {
    if (!system) return std::nullopt;
    for (const auto& device : system->devices()) {
      if (QString::fromStdString(device.identity_key) == identity && device.connected)
        return device.id;
    }
    return std::nullopt;
  }

  void persistAssignments() {
    if (!system) return;
    for (std::uint32_t user = 0; user < xenon::input::kMaxUsers; ++user) {
      const auto sources = system->sources_for_user(user);
      QStringList identities;
      for (const auto id : sources) {
        const auto info = system->device(id);
        if (info) identities.push_back(QString::fromStdString(info->identity_key));
      }
      settings.setValue(QStringLiteral("input/user%1/sources").arg(user), identities);
    }
  }

  void restoreAssignments() {
    if (!system) return;
    for (std::uint32_t user = 0; user < xenon::input::kMaxUsers; ++user) {
      const auto key = QStringLiteral("input/user%1/sources").arg(user);
      const auto identities = settings.value(key).toStringList();
      if (identities.isEmpty()) continue;
      bool primary = true;
      for (const auto& identity : identities) {
        const auto id = idForIdentity(identity);
        if (!id) continue;
        if (primary) {
          static_cast<void>(system->assign_user(user, *id));
          primary = false;
        } else {
          static_cast<void>(system->add_user_source(user, *id));
        }
      }
    }
  }

  void applyProfileDefaults() {
    if (!system) return;
    auto profile = system->profiles().profile("default");
    if (!profile) return;
    const auto deadzone = static_cast<float>(settings.numberValue(QStringLiteral("input/deadzone"), 0.10));
    profile->left_stick.inner_deadzone = std::clamp(deadzone, 0.0f, 0.5f);
    profile->right_stick.inner_deadzone = std::clamp(deadzone, 0.0f, 0.5f);
    static_cast<void>(system->profiles().upsert(*profile));
    system->set_background_input_policy(settings.boolValue(QStringLiteral("input/backgroundInput"), false)
        ? xenon::input::BackgroundInputPolicy::Always
        : xenon::input::BackgroundInputPolicy::ForegroundOnly);
  }

  bool configureSystem(const QString& backend) {
    system = std::make_unique<xenon::input::InputSystem>();

    auto add_sdl = [&]() {
      auto driver = xenon::input::create_sdl_input_driver();
      if (driver) static_cast<void>(system->add_driver(std::move(driver)));
    };
    auto add_xinput = [&]() {
      auto driver = xenon::input::create_xinput_driver();
      if (driver) static_cast<void>(system->add_driver(std::move(driver)));
    };

#if defined(Q_OS_WIN)
    if (backend == QStringLiteral("Native XInput")) add_xinput();
    else if (backend == QStringLiteral("SDL")) add_sdl();
    else {
      add_xinput();
      const auto result = system->setup();
      if (result == xenon::input::Result::Success) {
        module_api = std::make_unique<xenon::input::module_api::Provider>(*system);
        return true;
      }
      system.reset();
      module_api.reset();
      system = std::make_unique<xenon::input::InputSystem>();
      add_sdl();
    }
#else
    static_cast<void>(backend);
    add_sdl();
#endif

    const auto result = system->setup();
    if (result != xenon::input::Result::Success) {
      system.reset();
      module_api.reset();
      return false;
    }
    module_api = std::make_unique<xenon::input::module_api::Provider>(*system);
    return true;
  }

  void configureFrontendRouter() {
    if (!system) return;
    using namespace xenon::input;
    auto& router = system->frontend_router();
    router.clear();
    const FrontendInputSource source = FrontendInputSource::Gamepad;
    router.bind({source, GamepadButton::DpadUp, FrontendInputAction::Up});
    router.bind({source, GamepadButton::DpadDown, FrontendInputAction::Down});
    router.bind({source, GamepadButton::DpadLeft, FrontendInputAction::Left});
    router.bind({source, GamepadButton::DpadRight, FrontendInputAction::Right});
    router.bind({source, GamepadButton::A, FrontendInputAction::Confirm});
    router.bind({source, GamepadButton::B, FrontendInputAction::Cancel});
    router.bind({source, GamepadButton::X, FrontendInputAction::Menu});
    router.bind({source, GamepadButton::Y, FrontendInputAction::Search});
    router.bind({source, GamepadButton::Guide, FrontendInputAction::QuickCenter});
    router.bind({source, GamepadButton::LeftShoulder, FrontendInputAction::PageBack});
    router.bind({source, GamepadButton::RightShoulder, FrontendInputAction::PageForward});
    router.bind({source, GamepadButton::Start, FrontendInputAction::QuickCenter});
    frontend_previous_states.fill({});
    frontend_has_previous.fill(false);
  }
#endif
};

InputFeature::InputFeature(SettingsFeature& settings, PathService& paths,
                           bool test_mode, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, settings, paths, test_mode)) {}

InputFeature::~InputFeature() { shutdown(); }

ServiceResult InputFeature::initialize() {
#if XENON_LAUNCHER_RUNTIME_INPUT
  const auto backend = impl_->settings.stringValue(QStringLiteral("input/backend"), QStringLiteral("Automatic"));
  if (!impl_->configureSystem(backend)) {
    impl_->last_status = QStringLiteral("Input compiled, but no host input backend initialized");
    emit changed();
    return ServiceResult::success(QStringLiteral("Input unavailable"), impl_->last_status);
  }

  impl_->applyProfileDefaults();
  impl_->configureFrontendRouter();
  const auto store = impl_->storePath();
  const QFileInfo info{store};
  if (info.exists()) {
    static_cast<void>(impl_->system->profiles().load(std::filesystem::path(store.toStdString())));
  } else {
    QDir{}.mkpath(info.absolutePath());
    static_cast<void>(impl_->system->profiles().save(std::filesystem::path(store.toStdString())));
  }

  impl_->restoreAssignments();
  impl_->last_status = QStringLiteral("Input v1 connected");
  emit changed();
  return ServiceResult::success(QStringLiteral("Input ready"),
                                QStringLiteral("Live Xenon Input devices and module API v1 are connected to the launcher."));
#else
  impl_->last_status = QStringLiteral("Input support is not compiled in this build");
  emit changed();
  return ServiceResult::success(QStringLiteral("Input unavailable"), impl_->last_status);
#endif
}

QVariantList InputFeature::frontendActions() {
  QVariantList result;
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system) return result;
  using namespace xenon::input;
  for (std::uint32_t user = 0; user < kMaxUsers; ++user) {
    State state{};
    if (impl_->system->get_state(user, state) != Result::Success) continue;
    const auto previous = impl_->frontend_previous_states[user];
    const bool had_previous = impl_->frontend_has_previous[user];
    impl_->frontend_previous_states[user] = state.gamepad;
    impl_->frontend_has_previous[user] = true;
    if (!had_previous) continue;

    constexpr std::uint16_t buttons[] = {
        GamepadButton::DpadUp, GamepadButton::DpadDown,
        GamepadButton::DpadLeft, GamepadButton::DpadRight,
        GamepadButton::A, GamepadButton::B, GamepadButton::X,
        GamepadButton::Y, GamepadButton::Guide,
        GamepadButton::LeftShoulder, GamepadButton::RightShoulder,
        GamepadButton::Start};
    for (const auto button : buttons) {
      const bool was_down = (previous.buttons & button) != 0;
      const bool is_down = (state.gamepad.buttons & button) != 0;
      if (!was_down && is_down) {
        impl_->system->frontend_router().dispatch(
            FrontendInputSource::Gamepad, button, true, false);
      }
    }
    FrontendInputEvent event{};
    while (impl_->system->frontend_router().poll(event)) {
      QString action;
      switch (event.action) {
        case FrontendInputAction::Up: action = QStringLiteral("up"); break;
        case FrontendInputAction::Down: action = QStringLiteral("down"); break;
        case FrontendInputAction::Left: action = QStringLiteral("left"); break;
        case FrontendInputAction::Right: action = QStringLiteral("right"); break;
        case FrontendInputAction::Confirm: action = QStringLiteral("confirm"); break;
        case FrontendInputAction::Cancel: action = QStringLiteral("cancel"); break;
        case FrontendInputAction::Menu: action = QStringLiteral("menu"); break;
        case FrontendInputAction::QuickCenter: action = QStringLiteral("quickCenter"); break;
        case FrontendInputAction::PageBack: action = QStringLiteral("pageBack"); break;
        case FrontendInputAction::PageForward: action = QStringLiteral("pageForward"); break;
        case FrontendInputAction::Search: action = QStringLiteral("search"); break;
        default: break;
      }
      if (!action.isEmpty()) result.append(action);
    }
  }
#endif
  return result;
}

void InputFeature::shutdown() noexcept {
#if XENON_LAUNCHER_RUNTIME_INPUT
  impl_->frontend_previous_states.fill({});
  impl_->frontend_has_previous.fill(false);
  if (impl_->system) {
    const auto store = impl_->storePath();
    const QFileInfo info{store};
    QDir{}.mkpath(info.absolutePath());
    static_cast<void>(impl_->system->profiles().save(std::filesystem::path(store.toStdString())));
    impl_->system->shutdown();
    impl_->module_api.reset();
    impl_->system.reset();
  }
#endif
}

bool InputFeature::available() const noexcept {
#if XENON_LAUNCHER_RUNTIME_INPUT
  return impl_->system != nullptr;
#else
  return false;
#endif
}

QString InputFeature::status() const { return impl_->last_status; }

QVariantList InputFeature::devices() const {
  QVariantList result;
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system) return result;
  for (const auto& device : impl_->system->devices()) {
    QVariantMap item;
    item.insert(QStringLiteral("id"), QString::number(device.id));
    item.insert(QStringLiteral("ordinal"), static_cast<int>(device.ordinal));
    item.insert(QStringLiteral("identityKey"), QString::fromStdString(device.identity_key));
    item.insert(QStringLiteral("driver"), QString::fromStdString(device.driver_name));
    item.insert(QStringLiteral("name"), QString::fromStdString(device.name));
    item.insert(QStringLiteral("subtype"), subtypeName(static_cast<int>(device.subtype)));
    item.insert(QStringLiteral("connection"), connectionName(static_cast<int>(device.connection)));
    item.insert(QStringLiteral("vendorId"), QStringLiteral("%1").arg(device.vendor_id, 4, 16, QLatin1Char('0')).toUpper());
    item.insert(QStringLiteral("productId"), QStringLiteral("%1").arg(device.product_id, 4, 16, QLatin1Char('0')).toUpper());
    item.insert(QStringLiteral("connected"), device.connected);
    item.insert(QStringLiteral("supportsVibration"), device.supports_vibration);
    item.insert(QStringLiteral("supportsPower"), device.supports_power_info);
    item.insert(QStringLiteral("supportsPlayerIndicator"), device.supports_player_indicator);
    if (device.power_valid) {
      item.insert(QStringLiteral("batteryPercent"), device.power.percentage == 0xFFu ? -1 : device.power.percentage);
    } else {
      item.insert(QStringLiteral("batteryPercent"), -1);
    }
    result.append(item);
  }
#endif
  return result;
}

QVariantList InputFeature::users() const {
  QVariantList result;
#if XENON_LAUNCHER_RUNTIME_INPUT
  for (std::uint32_t user = 0; user < xenon::input::kMaxUsers; ++user) {
    QVariantMap row;
    row.insert(QStringLiteral("userIndex"), static_cast<int>(user));
    QVariantList sources;
    if (impl_->system) {
      for (const auto id : impl_->system->sources_for_user(user)) {
        const auto device = impl_->system->device(id);
        if (!device) continue;
        sources.append(QVariantMap{{QStringLiteral("id"), QString::number(id)},
                                   {QStringLiteral("identityKey"), QString::fromStdString(device->identity_key)},
                                   {QStringLiteral("name"), QString::fromStdString(device->name)},
                                   {QStringLiteral("subtype"), subtypeName(static_cast<int>(device->subtype))}});
      }
      const auto primary = impl_->system->device_for_user(user);
      if (primary) {
        const auto device = impl_->system->device(*primary);
        if (device) {
          row.insert(QStringLiteral("deviceId"), QString::number(*primary));
          row.insert(QStringLiteral("identityKey"), QString::fromStdString(device->identity_key));
          row.insert(QStringLiteral("deviceName"), QString::fromStdString(device->name));
        }
      }
      const auto profile_id = Impl::profileIdForUser(*impl_->system, user);
      row.insert(QStringLiteral("profileId"), profile_id);
    }
    row.insert(QStringLiteral("sources"), sources);
    result.append(row);
  }
#endif
  return result;
}

QVariantList InputFeature::profiles() const {
  QVariantList result;
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system) return result;
  for (const auto& profile : impl_->system->profiles().profiles()) {
    result.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(profile.id)},
                              {QStringLiteral("name"), QString::fromStdString(profile.name)},
                              {QStringLiteral("enabled"), profile.enabled},
                              {QStringLiteral("leftDeadzone"), profile.left_stick.inner_deadzone},
                              {QStringLiteral("rightDeadzone"), profile.right_stick.inner_deadzone}});
  }
#endif
  return result;
}

QVariantMap InputFeature::diagnostics() const {
  QVariantMap result;
  result.insert(QStringLiteral("available"), available());
  result.insert(QStringLiteral("status"), status());
  result.insert(QStringLiteral("profileStorePath"), profileStorePath());
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (impl_->system) {
    const auto diagnostics = impl_->system->diagnostics();
    result.insert(QStringLiteral("setup"), diagnostics.setup);
    result.insert(QStringLiteral("enabled"), diagnostics.enabled);
    result.insert(QStringLiteral("focused"), diagnostics.focused);
    result.insert(QStringLiteral("effectiveActive"), diagnostics.effective_active);
    result.insert(QStringLiteral("driverCount"), static_cast<qlonglong>(diagnostics.driver_count));
    result.insert(QStringLiteral("connectedDeviceCount"), static_cast<qlonglong>(diagnostics.connected_device_count));
    result.insert(QStringLiteral("assignedUserCount"), static_cast<qlonglong>(diagnostics.assigned_user_count));
  }
#endif
  return result;
}

QVariantMap InputFeature::moduleApiInfo() const {
  QVariantMap result;
  result.insert(QStringLiteral("name"), QStringLiteral("Xenon Input Module API"));
  result.insert(QStringLiteral("id"), QStringLiteral("input"));
  result.insert(QStringLiteral("version"), 1);
  result.insert(QStringLiteral("header"), QStringLiteral("xenon/input/module_api.hpp"));
  result.insert(QStringLiteral("cmakeTarget"), QStringLiteral("Xenon::InputAPI"));
  result.insert(QStringLiteral("manifestKey"), QStringLiteral("runtimeApis.input"));
  result.insert(QStringLiteral("guestXamBridge"), true);
  result.insert(QStringLiteral("features"), QStringList{
      QStringLiteral("state"), QStringLiteral("capabilities"), QStringLiteral("vibration"),
      QStringLiteral("power"), QStringLiteral("device-metadata"), QStringLiteral("profiles"),
      QStringLiteral("multi-source"), QStringLiteral("flight-devices")});
  return result;
}

QString InputFeature::profileStorePath() const {
#if XENON_LAUNCHER_RUNTIME_INPUT
  return impl_->storePath();
#else
  return {};
#endif
}

ServiceResult InputFeature::refresh() {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system) return ServiceResult::failure(QStringLiteral("Input unavailable"), impl_->last_status);
  impl_->system->refresh_devices();
  impl_->frontend_previous_states.fill({});
  impl_->frontend_has_previous.fill(false);
  impl_->restoreAssignments();
  emit changed();
  return ServiceResult::success(QStringLiteral("Input refreshed"), QStringLiteral("Connected input devices were rescanned."));
#else
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not compiled in this build."));
#endif
}

ServiceResult InputFeature::reconfigure() {
  shutdown();
  return initialize();
}

ServiceResult InputFeature::applySettings() {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system) return ServiceResult::failure(QStringLiteral("Input unavailable"), impl_->last_status);
  impl_->applyProfileDefaults();
  static_cast<void>(impl_->system->profiles().save(std::filesystem::path(impl_->storePath().toStdString())));
  emit changed();
  return ServiceResult::success();
#else
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::assignUser(int user_index, const QString& identity_key) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input assignment"), QStringLiteral("Invalid input user."));
  const auto id = impl_->idForIdentity(identity_key);
  if (!id) return ServiceResult::failure(QStringLiteral("Input assignment"), QStringLiteral("The selected device is not connected."));
  const auto status = impl_->system->assign_user(static_cast<std::uint32_t>(user_index), *id);
  if (status != xenon::input::Result::Success)
    return ServiceResult::failure(QStringLiteral("Input assignment"), QString::fromLatin1(xenon::input::to_string(status).data(), static_cast<qsizetype>(xenon::input::to_string(status).size())));
  impl_->persistAssignments();
  emit changed();
  return ServiceResult::success(QStringLiteral("Controller assigned"), QStringLiteral("Xbox user %1 now uses the selected device.").arg(user_index + 1));
#else
  static_cast<void>(user_index); static_cast<void>(identity_key);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not compiled in this build."));
#endif
}

ServiceResult InputFeature::clearUser(int user_index) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input assignment"), QStringLiteral("Invalid input user."));
  static_cast<void>(impl_->system->clear_user(static_cast<std::uint32_t>(user_index)));
  impl_->persistAssignments();
  emit changed();
  return ServiceResult::success(QStringLiteral("Controller cleared"));
#else
  static_cast<void>(user_index);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::addUserSource(int user_index, const QString& identity_key) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("Invalid input user."));
  const auto id = impl_->idForIdentity(identity_key);
  if (!id) return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("The selected device is not connected."));
  const auto status = impl_->system->add_user_source(static_cast<std::uint32_t>(user_index), *id);
  if (status != xenon::input::Result::Success) return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("The source could not be added."));
  impl_->persistAssignments();
  emit changed();
  return ServiceResult::success(QStringLiteral("Input source added"));
#else
  static_cast<void>(user_index); static_cast<void>(identity_key);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::removeUserSource(int user_index, const QString& identity_key) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("Invalid input user."));
  const auto id = impl_->idForIdentity(identity_key);
  if (!id) return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("The selected device is not connected."));
  const auto status = impl_->system->remove_user_source(static_cast<std::uint32_t>(user_index), *id);
  if (status != xenon::input::Result::Success) return ServiceResult::failure(QStringLiteral("Input source"), QStringLiteral("The source was not assigned."));
  impl_->persistAssignments();
  emit changed();
  return ServiceResult::success(QStringLiteral("Input source removed"));
#else
  static_cast<void>(user_index); static_cast<void>(identity_key);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::bindUserProfile(int user_index, const QString& profile_id) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input profile"), QStringLiteral("Invalid input user."));
  if (!impl_->system->profiles().bind_user(static_cast<std::uint32_t>(user_index), profile_id.toStdString()))
    return ServiceResult::failure(QStringLiteral("Input profile"), QStringLiteral("The requested input profile does not exist."));
  static_cast<void>(impl_->system->profiles().save(std::filesystem::path(impl_->storePath().toStdString())));
  emit changed();
  return ServiceResult::success(QStringLiteral("Input profile assigned"));
#else
  static_cast<void>(user_index); static_cast<void>(profile_id);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::clearUserProfile(int user_index) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Input profile"), QStringLiteral("Invalid input user."));
  static_cast<void>(impl_->system->profiles().clear_user(static_cast<std::uint32_t>(user_index)));
  static_cast<void>(impl_->system->profiles().save(std::filesystem::path(impl_->storePath().toStdString())));
  emit changed();
  return ServiceResult::success(QStringLiteral("Input profile cleared"));
#else
  static_cast<void>(user_index);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

ServiceResult InputFeature::testVibration(int user_index) {
#if XENON_LAUNCHER_RUNTIME_INPUT
  if (!impl_->system || user_index < 0 || user_index >= static_cast<int>(xenon::input::kMaxUsers))
    return ServiceResult::failure(QStringLiteral("Rumble test"), QStringLiteral("Invalid input user."));
  if (!impl_->settings.boolValue(QStringLiteral("input/rumble"), true))
    return ServiceResult::failure(QStringLiteral("Rumble disabled"), QStringLiteral("Enable controller rumble before testing it."));
  const auto status = impl_->system->set_vibration(static_cast<std::uint32_t>(user_index), {26000, 16000});
  if (status != xenon::input::Result::Success)
    return ServiceResult::failure(QStringLiteral("Rumble unavailable"), QStringLiteral("The selected device does not support vibration."));
  QTimer::singleShot(300, this, [this, user_index]() {
    if (impl_->system) static_cast<void>(impl_->system->set_vibration(static_cast<std::uint32_t>(user_index), {}));
  });
  return ServiceResult::success(QStringLiteral("Rumble test"), QStringLiteral("A short vibration test was sent."));
#else
  static_cast<void>(user_index);
  return ServiceResult::failure(QStringLiteral("Input unavailable"), QStringLiteral("Input support is not available in this build or session."));
#endif
}

}  // namespace xenon::launcher::frontend_backend
