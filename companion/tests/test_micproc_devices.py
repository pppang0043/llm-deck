import micproc


class Devices:
    def __init__(self, usb):
        self.devices = [{"name": "Realtek", "hostapi": 1, "max_input_channels": 1}]
        if usb:
            self.devices += [{"name": "USB", "hostapi": 0, "max_input_channels": 1}]

    def query_hostapis(self, index):
        return {"name": ["MME", "Windows WDM-KS"][index]}

    def query_devices(self, index=None, kind=None):
        return self.devices[0] if kind else self.devices if index is None else self.devices[index]


def test_only_wdm_ks_is_not_an_input():
    server = micproc.MicServer(Devices(False), lambda msg: None)
    assert server._inputs() == []
    assert server._default() == ""
    assert server._index("Realtek") is None


def test_usb_is_usable_even_when_default_is_wdm_ks():
    server = micproc.MicServer(Devices(True), lambda msg: None)
    assert server._inputs() == ["USB"]
    assert server._default() == ""
    assert server._index("USB") == 1
    assert server._index("Realtek") is None
