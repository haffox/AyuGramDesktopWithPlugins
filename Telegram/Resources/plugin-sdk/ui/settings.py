from dataclasses import dataclass
from typing import Callable, Optional

@dataclass
class Header:
    text: str

@dataclass
class Input:
    key: str
    text: str
    default: str = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable] = None

@dataclass
class Switch:
    key: str
    text: str
    default: bool = False
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable] = None

@dataclass
class Text:
    text: str
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_click: Optional[Callable] = None
