#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/input/flight_driver.hpp"
#include "xenon/input/state_merge.hpp"
#include "xenon/input/system.hpp"
#include "xenon/input/xinput_driver.hpp"

namespace input = xenon::input;
namespace {

class StaticDriver final : public input::InputDriver {
 public:
  std::string_view name() const noexcept override { return "static"; }
  input::Result setup() override { return input::Result::Success; }
  void shutdown() noexcept override {}
  void enumerate_devices(std::vector<input::DriverDeviceInfo>& out) override { out=devices; }
  input::Result get_state(input::NativeDeviceId id,input::GamepadState& out) override { auto it=states.find(id); if(it==states.end())return input::Result::DeviceNotConnected; out=it->second; return input::Result::Success; }
  input::Result get_capabilities(input::NativeDeviceId id,input::Capabilities& out) override { if(!states.contains(id))return input::Result::DeviceNotConnected; out={}; return input::Result::Success; }
  input::Result set_vibration(input::NativeDeviceId,const input::Vibration&) override { return input::Result::Success; }
  input::Result get_keystroke(input::NativeDeviceId id,input::Keystroke& out) override { auto& q=keys[id]; if(q.empty())return input::Result::Empty; out=q.front();q.pop_front();return input::Result::Success; }
  std::vector<input::DriverDeviceInfo> devices{};
  std::unordered_map<input::NativeDeviceId,input::GamepadState> states{};
  std::unordered_map<input::NativeDeviceId,std::deque<input::Keystroke>> keys{};
};

input::DriverDeviceInfo dev(input::NativeDeviceId id,const char* key){ input::DriverDeviceInfo d{}; d.native_id=id;d.persistent_key=key;d.name=key;d.type=input::DeviceType::Gamepad;d.subtype=input::DeviceSubtype::Gamepad;d.supports_keystrokes=true;return d; }

class FakeXInputHost final : public input::XInputHost {
 public:
  input::Result setup() override { return input::Result::Success; }
  void shutdown() noexcept override {}
  input::Result enumerate(std::vector<input::XInputHostDevice>& out) override { out=devices; return input::Result::Success; }
  input::Result get_state(input::NativeDeviceId id,input::GamepadState& out) override { if(id!=1)return input::Result::DeviceNotConnected;out=state;return input::Result::Success; }
  input::Result get_capabilities(input::NativeDeviceId id,input::Capabilities& out) override { if(id!=1)return input::Result::DeviceNotConnected;out={};out.subtype=input::DeviceSubtype::FlightStick;out.flags=input::CapabilityVibrationSupported|input::CapabilityKeystrokeSupported;return input::Result::Success; }
  input::Result set_vibration(input::NativeDeviceId id,const input::Vibration& v) override { if(id!=1)return input::Result::DeviceNotConnected;last_vibration=v;return input::Result::Success; }
  input::Result get_keystroke(input::NativeDeviceId id,input::Keystroke& out) override { if(id!=1)return input::Result::DeviceNotConnected;if(keys.empty())return input::Result::Empty;out=keys.front();keys.pop_front();return input::Result::Success; }
  input::Result get_power_info(input::NativeDeviceId id,input::PowerInfo& out) override { if(id!=1)return input::Result::DeviceNotConnected;out={input::PowerSource::Battery,input::PowerLevel::Full,100,false};return input::Result::Success; }
  std::vector<input::XInputHostDevice> devices{{1,"slot:0","Native XInput",input::DeviceSubtype::FlightStick,true,true,true}};
  input::GamepadState state{}; input::Vibration last_vibration{}; std::deque<input::Keystroke> keys{};
};

void test_multi_source_merge_and_keystroke() {
  input::InputSystem system;
  auto driver=std::make_unique<StaticDriver>(); auto* d=driver.get();
  d->devices={dev(1,"pad"),dev(2,"keyboard")};
  d->states[1].buttons=input::A; d->states[1].thumb_lx=10000; d->states[1].left_trigger=20;
  d->states[2].buttons=input::B; d->states[2].thumb_lx=-20000; d->states[2].left_trigger=200;
  d->keys[2].push_back({0x5801,0,input::KeystrokeKeyDown,0,0});
  assert(system.add_driver(std::move(driver))); assert(system.setup()==input::Result::Success);
  auto devices=system.devices(); assert(devices.size()==2);
  assert(system.assign_user(0,devices[0].id)==input::Result::Success);
  assert(system.add_user_source(0,devices[1].id)==input::Result::Success);
  assert(system.sources_for_user(0).size()==2);
  input::State state{}; assert(system.get_state(0,state)==input::Result::Success);
  assert((state.gamepad.buttons&(input::A|input::B))==(input::A|input::B));
  assert(state.gamepad.thumb_lx==-20000); assert(state.gamepad.left_trigger==200);
  const auto packet=state.packet_number; assert(system.get_state(0,state)==input::Result::Success&&state.packet_number==packet);
  input::Keystroke key{}; assert(system.get_keystroke(0,0,key)==input::Result::Success); assert(key.virtual_key==0x5801&&key.user_index==0);
  assert(system.remove_user_source(0,devices[1].id)==input::Result::Success);
  assert(system.get_state(0,state)==input::Result::Success); assert(state.gamepad.buttons==input::A);
}

void test_flight_mapping() {
  input::FlightInputDriver flight; assert(flight.setup()==input::Result::Success);
  assert(flight.connect(7,"hotas-7","HOTAS")==input::Result::Success);
  input::FlightState raw{}; raw.roll=.5f; raw.pitch=.25f; raw.yaw=-.75f; raw.throttle=.8f; raw.hat_y=1; raw.buttons=1ull<<0;
  assert(flight.update(7,raw)==input::Result::Success);
  input::GamepadState state{}; assert(flight.get_state(7,state)==input::Result::Success);
  assert(state.thumb_lx>15000); assert(state.thumb_ly<0); assert(state.thumb_rx<0); assert(state.right_trigger>190); assert(state.left_trigger==0); assert((state.buttons&input::DpadUp)!=0); assert((state.buttons&input::A)!=0);
  std::vector<input::DriverDeviceInfo> devices; flight.enumerate_devices(devices); assert(devices.size()==1&&devices[0].subtype==input::DeviceSubtype::FlightStick);
}

void test_xinput_driver_contract() {
  auto host=std::make_unique<FakeXInputHost>(); auto* fake=host.get(); fake->state.buttons=input::Y; fake->state.thumb_rx=12345; fake->keys.push_back({0x5803,0,input::KeystrokeKeyDown,0,0});
  input::XInputDriver driver(std::move(host)); assert(driver.setup()==input::Result::Success);
  std::vector<input::DriverDeviceInfo> devices; driver.enumerate_devices(devices); assert(devices.size()==1&&devices[0].persistent_key=="slot:0");
  input::GamepadState state{};assert(driver.get_state(1,state)==input::Result::Success&&state.buttons==input::Y);
  input::Capabilities caps{};assert(driver.get_capabilities(1,caps)==input::Result::Success&&caps.subtype==input::DeviceSubtype::FlightStick);
  assert(driver.set_vibration(1,{111,222})==input::Result::Success&&fake->last_vibration.right_motor_speed==222);
  input::Keystroke key{};assert(driver.get_keystroke(1,key)==input::Result::Success&&key.virtual_key==0x5803);
  input::PowerInfo power{};assert(driver.get_power_info(1,power)==input::Result::Success&&power.percentage==100);
}

void test_merge_helper_and_hotplug_stress() {
  input::GamepadState a{};a.buttons=input::X;a.thumb_ry=100;input::GamepadState b{};b.buttons=input::Y;b.thumb_ry=-200;input::merge_gamepad_state(a,b);assert((a.buttons&(input::X|input::Y))==(input::X|input::Y)&&a.thumb_ry==-200);
  input::InputSystem system;auto driver=std::make_unique<StaticDriver>();auto* d=driver.get();d->devices={dev(1,"stress")};d->states[1]={};assert(system.add_driver(std::move(driver)));assert(system.setup()==input::Result::Success);const auto stable=system.devices()[0].id;
  for(int i=0;i<500;++i){d->devices.clear();system.refresh_devices();assert(!system.device(stable)->connected);d->devices={dev(static_cast<input::NativeDeviceId>(i+2),"stress")};d->states[i+2]={};system.refresh_devices();assert(system.device(stable)->connected);}
  assert(system.device(stable)->id==stable);
}
}

int main(){test_multi_source_merge_and_keystroke();test_flight_mapping();test_xinput_driver_contract();test_merge_helper_and_hotplug_stress();return 0;}
