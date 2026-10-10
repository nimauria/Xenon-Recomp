# XAM services and exports. XAM and XenonSession depend on each other
# (XamSession::register_exports(registry, session)), so these sources build
# into xenon_core rather than a library of their own.
target_sources(xenon_core PRIVATE
  user_manager.cpp
  locale_manager.cpp
  content_manager.cpp
  content_graph.cpp
  title_update_manager.cpp
  dlc_manager.cpp
  save_manager.cpp
  notification_manager.cpp
  achievement_manager.cpp
  guest_enumerator.cpp
  xam_session.cpp
  xam_user_exports.cpp
  xam_locale_exports.cpp
  xam_content_exports.cpp
  xam_notification_exports.cpp
  xam_achievement_exports.cpp
  xam_system_exports.cpp
  net/xam_net_exports.cpp
  net/socket_setup.cpp
  net/transfer.cpp
  net/listen.cpp
  net/xnet.cpp
  net/xam_net_internal.hpp
  # xam_socket_manager.cpp backs XamSocketManager (used by net/xam_net_exports.cpp
  # since that file's std::make_shared<XamSocketManager>() call), but was
  # missing from this source list - any executable linking xenon_core that
  # actually instantiates a session (and therefore register_net_exports())
  # would fail to link without it.
  xam_socket_manager.cpp
  xam_voice_exports.cpp
  xam_task_exports.cpp
  xam_msg_exports.cpp
  xam_enum_exports.cpp
  xam_session_handle_exports.cpp
  xam_ui_exports.cpp
)
