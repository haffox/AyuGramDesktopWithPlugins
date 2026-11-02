import ayugram_api as _host
from base_plugin import UnsupportedFeature

def log(data):
    _host.log("DEBUG", str(data))

def run_on_ui_thread(func, delay=0):
    raise UnsupportedFeature("Python callbacks stay on worker; use declarative native UI APIs, not an Android UI thread")
