from typing import Callable, Literal, TypedDict
API_VERSION: int
DROP: Literal[False]
class MessageData(TypedDict):
    peer_id: str
    message_id: str
    text: str
    outgoing: bool
    service: bool
    plugin_origin: bool
class CommandData(MessageData):
    arguments: str

def send_message(peer_id: str, text: str, /) -> None: ...
def show_toast(text: str, /) -> None: ...
def register_command(command: str, callback: Callable[[CommandData], object], /) -> None: ...
def add_context_menu_item(item_id: str, label: str, callback: Callable[[MessageData], object], /) -> None: ...

# Compatibility extension; account token is NOT an Android account index.
def get_account_epoch() -> int: ...
def log(level: str, text: str, /) -> None: ...
