"""Explicit, small exteraGram compatibility subset; NOT Android emulation."""
from dataclasses import dataclass
from enum import Enum, auto
from typing import Any, Callable, Optional
import logging
import ayugram_api as _host


class UnsupportedFeature(NotImplementedError):
    pass


class HookStrategy(Enum):
    DEFAULT = auto()
    CANCEL = auto()
    MODIFY = auto()
    MODIFY_FINAL = auto()


@dataclass
class HookResult:
    strategy: HookStrategy = HookStrategy.DEFAULT
    params: Any = None
    request: Any = None
    response: Any = None
    update: Any = None
    updates: Any = None


class MenuItemType(Enum):
    MESSAGE_CONTEXT_MENU = auto()
    DRAWER_MENU = auto()
    MAIN_MENU = auto()
    CHAT_ACTION_MENU = auto()
    PROFILE_ACTION_MENU = auto()


@dataclass
class MenuItemData:
    menu_type: MenuItemType
    text: str
    on_click: Callable
    item_id: Optional[str] = None
    icon: Optional[str] = None
    subtext: Optional[str] = None
    condition: Optional[str] = None
    priority: int = 0


class _HostLogHandler(logging.Handler):
    def emit(self, record):
        _host.log(record.levelname, self.format(record))


def _logger(owner):
    logger = logging.getLogger("ayu.extera." + owner)
    logger.setLevel(logging.DEBUG)
    logger.propagate = False
    if not logger.handlers:
        logger.addHandler(_HostLogHandler())
    return logger


@dataclass(frozen=True)
class _MessageSnapshot:
    """DTO facade, NOT org.telegram.messenger.MessageObject."""
    peer_id: str
    message_id: str
    messageText: str

    def getId(self) -> str:
        # Intentionally no int conversion: the desktop host owns ID semantics.
        return self.message_id


def _menu_context(msg):
    return {
        "account": _host.get_account_epoch(),
        "dialog_id": msg["peer_id"],
        "message": _MessageSnapshot(msg["peer_id"], msg["message_id"], msg["text"]),
    }


class BasePlugin:
    def __init__(self):
        self._send_hook = False
        self._menu_ids = set()
        self._defaults = {}
        self._desktop_owner = "unbound"
        self.logger = _logger("unbound")

    def on_plugin_load(self):
        pass

    def on_plugin_unload(self):
        pass

    def create_settings(self):
        return []

    def add_on_send_message_hook(self):
        self._send_hook = True

    def get_setting(self, key, default=None):
        # Read-only schema defaults: no claim of persistent Android settings.
        return self._defaults.get(key, default)

    def set_setting(self, key, value, reload_settings=False):
        raise UnsupportedFeature("Persistent settings and native Qt editor are not implemented in this PoC")

    def export_settings(self):
        raise UnsupportedFeature("Persistent settings export is not implemented")

    def import_settings(self, settings, reload_settings=True):
        raise UnsupportedFeature("Persistent settings import is not implemented")

    def add_hook(self, *args, **kwargs):
        raise UnsupportedFeature("TL request/update interception requires a native TDesktop adapter; not emulated")

    def hook_method(self, *args, **kwargs):
        raise UnsupportedFeature("Java/Xposed methods do not exist in the C++/Qt process")

    hook_all_methods = hook_method
    hook_all_constructors = hook_method

    def add_menu_item(self, item):
        if not isinstance(item, MenuItemData):
            raise TypeError("Expected MenuItemData")
        if item.menu_type is not MenuItemType.MESSAGE_CONTEXT_MENU:
            raise UnsupportedFeature("Only MESSAGE_CONTEXT_MENU has a desktop bridge")
        if item.icon is not None or item.subtext is not None or item.condition is not None or item.priority != 0:
            raise UnsupportedFeature("Android icon/subtext/MVEL/priority need explicit native UI mapping")
        key = item.item_id or "menu_" + str(len(self._menu_ids) + 1)
        if key in self._menu_ids:
            raise ValueError("Duplicate menu ID")
        if not callable(item.on_click):
            raise TypeError("Menu callback must be callable")
        _host.add_context_menu_item(key, item.text, lambda msg: item.on_click(_menu_context(msg)))
        self._menu_ids.add(key)
        return key

    def remove_menu_item(self, item_id):
        raise UnsupportedFeature("Dynamic menu removal needs an SDK unregister operation; reload removes owned items")

    def client(self, account=None):
        from client_utils import get_client
        return get_client(account)
