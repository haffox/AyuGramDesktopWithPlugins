import ayugram_api as _host
from base_plugin import UnsupportedFeature

class BulletinHelper:
    @staticmethod
    def _text(text, fragment=None):
        if fragment is not None:
            raise UnsupportedFeature("Android fragments are unavailable")
        return _host.show_toast(text)

    # Explicit visual downgrade: same Qt toast, no Android Lottie/style parity.
    show_info = _text
    show_error = _text
    show_success = _text
