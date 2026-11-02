"""Called by CPython loader under its registration scope and GIL."""
from types import SimpleNamespace
from base_plugin import BasePlugin, HookResult, HookStrategy, UnsupportedFeature, _logger
import ayugram_api as _host


def attach(module, host_id):
    candidates = [value for value in vars(module).values()
                  if isinstance(value, type) and value is not BasePlugin
                  and issubclass(value, BasePlugin) and value.__module__ == module.__name__]
    candidates = list(dict.fromkeys(candidates))
    if not candidates:
        return False  # Existing native ayugram_api modules are unchanged.
    if len(candidates) != 1:
        raise UnsupportedFeature("Exactly one locally defined BasePlugin subclass is required")
    if any(name in vars(module) for name in ("on_message_send", "on_message_received", "on_ui_init")):
        raise UnsupportedFeature("Mixed native and Extera hooks require an explicit wrapper")
    if getattr(module, "__requirements__", None):
        raise UnsupportedFeature("Automatic pip/dependency installation is intentionally disabled")
    plugin = candidates[0]()
    plugin._desktop_owner = host_id
    plugin.logger = _logger(host_id)
    module._ayu_extera_instance = plugin
    module._ayu_extera_unload = plugin.on_plugin_unload

    plugin.on_plugin_load()
    schema = plugin.create_settings()
    if not isinstance(schema, (tuple, list)):
        raise TypeError("create_settings() must return a list or tuple")
    from ui.settings import Header, Input, Text, Switch
    for row in schema:
        if type(row) not in (Header, Input, Text, Switch):
            raise UnsupportedFeature("Unsupported settings row; Android custom views are not emulated")
        if getattr(row, "on_change", None) is not None or getattr(row, "on_click", None) is not None:
            raise UnsupportedFeature("Settings callbacks require the native Qt settings renderer")
        if isinstance(row, (Input, Switch)):
            plugin._defaults[row.key] = row.default
    module._ayu_extera_settings_schema = tuple(schema)
    if schema:
        plugin.logger.warning("PoC settings: schema/defaults only; no persistent editor")
    plugin.logger.warning("Extera subset mode: Android app/sdk version constraints are not desktop compatibility checks")

    def outgoing(text, peer_id):
        if not plugin._send_hook:
            return None
        params = SimpleNamespace(message=text, peer=peer_id)
        result = plugin.on_send_message_hook(_host.get_account_epoch(), params)
        if not isinstance(result, HookResult):
            raise TypeError("on_send_message_hook must return HookResult")
        if result.strategy is HookStrategy.DEFAULT:
            return None  # Uncommitted mutation must NOT leak out.
        if result.strategy is HookStrategy.CANCEL:
            return _host.DROP
        if result.strategy is HookStrategy.MODIFY_FINAL:
            raise UnsupportedFeature("MODIFY_FINAL needs a stop-chain result in the native engine; not approximated")
        if result.strategy is not HookStrategy.MODIFY:
            raise TypeError("Unknown hook strategy")
        modified = result.params
        if not isinstance(modified, SimpleNamespace):
            raise UnsupportedFeature("Only desktop text SendMessageParams snapshots are supported")
        if set(vars(modified)) != {"message", "peer"} or modified.peer != peer_id:
            raise UnsupportedFeature("Changing recipient/options/media requires a native send-intent adapter")
        if not isinstance(modified.message, str):
            raise TypeError("Modified message must be str")
        return modified.message

    if plugin._send_hook:
        if not callable(getattr(plugin, "on_send_message_hook", None)):
            raise TypeError("Send hook registered but method is missing")
        module.on_message_send = outgoing
    return True
