"""Text-only subset. Peer/account conversions must never be guessed."""
import ayugram_api as _host
from base_plugin import UnsupportedFeature


def _account(account):
    current = _host.get_account_epoch()
    if account is not None and (type(account) is not int or account != current):
        raise UnsupportedFeature("Account must be the opaque desktop account token received by the hook")
    return current


def send_text(peer_id, text, *, account=None, **options):
    _account(account)
    if options:
        raise UnsupportedFeature("replyToMsg/parse_mode/scheduling/media need a richer native send API")
    if not isinstance(peer_id, str) or not peer_id:
        raise UnsupportedFeature("Android numeric dialog IDs need an explicit account-scoped peer resolver; pass the desktop handle")
    if not isinstance(text, str):
        raise TypeError("text must be str")
    return _host.send_message(peer_id, text)


def send_message(params, parse_mode=None, *, account=None):
    if parse_mode is not None or not isinstance(params, dict) or set(params) != {"peer", "message"}:
        raise UnsupportedFeature("Only plain {peer, message} is supported, without parse_mode")
    return send_text(params["peer"], params["message"], account=account)


def get_hook_account():
    return _host.get_account_epoch()


class AccountClient:
    def __init__(self, account):
        self.account = _account(account)

    def send_text(self, peer_id, text, **options):
        return send_text(peer_id, text, account=self.account, **options)

    def send_message(self, params, parse_mode=None):
        return send_message(params, parse_mode, account=self.account)


def get_client(account=None):
    return AccountClient(account)
