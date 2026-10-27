import ctypes as ct
from types import SimpleNamespace as NS

import pytest

import wingpu


def engine(pid, number=0, luid="0x0_0x123", physical=0, kind="3D"):
    return f"pid_{pid}_luid_{luid}_phys_{physical}_eng_{number}_engtype_{kind}"


def test_aggregate_same_engine_across_processes_not_across_gpus():
    assert wingpu._busiest_engine([
        (engine(1), 40), (engine(2), 30),
        (engine(3, number=1, kind="Copy"), 60),
        (engine(4, luid="0x0_0x456"), 50),
    ]) == 70
    assert wingpu._busiest_engine([(engine(1), 80), (engine(2), 50)]) == 100


def test_unknown_invalid_and_idle():
    assert wingpu._busiest_engine([("unknown", 70), (engine(1), float("nan")), (engine(2), -1)]) is None
    assert wingpu._busiest_engine([(engine(1), 0)]) == 0


class FakePdh:
    def __init__(self, fail_add=False, fail_collect=False):
        self.closed = []
        self.opens = 0
        self.collects = 0
        self.fail_add = fail_add
        self.fail_collect = fail_collect
        self.names = [ct.create_unicode_buffer(engine(i)) for i in (1, 2, 3)]
        self.api = NS(PdhOpenQueryW=self.function(self.open), PdhAddEnglishCounterW=self.function(self.add),
                      PdhCollectQueryData=self.function(self.collect),
                      PdhGetFormattedCounterArrayW=self.function(self.array),
                      PdhCloseQuery=self.function(lambda query: self.closed.append(query.value)))

    @staticmethod
    def function(fn):
        def call(*args):
            return fn(*args)
        return call

    def open(self, source, user, query):
        self.opens += 1
        query._obj.value = 100
        return 0

    def add(self, query, path, user, counter):
        assert path == r"\GPU Engine(*)\Utilization Percentage"
        counter._obj.value = 200
        return int(self.fail_add)

    def collect(self, query):
        self.collects += 1
        return int(self.fail_collect)

    def array(self, counter, format, size, count, items):
        if items is None:
            size._obj.value = 3 * ct.sizeof(wingpu._Item)
            return wingpu._MORE_DATA
        count._obj.value = 3
        for i, (status, value) in enumerate([(0, 30), (1, 40), (0xC0000BBA, 99)]):
            items[i].name = ct.cast(self.names[i], ct.c_wchar_p)
            items[i].value.status = status
            items[i].value.data.doubleValue = value
        return 0


def make(monkeypatch, **kwargs):
    fake = FakePdh(**kwargs)
    monkeypatch.setattr(wingpu.sys, "platform", "win32")
    monkeypatch.setattr(wingpu.ct, "WinDLL", lambda name: fake.api, raising=False)
    monkeypatch.setattr(wingpu.atexit, "register", lambda fn: None)
    return wingpu.WindowsGpu(), fake


def test_pdh_warmup_valid_statuses_and_cleanup(monkeypatch):
    gpu, api = make(monkeypatch)
    assert gpu.percent(100) is None  # first rate sample
    assert gpu.percent(103) == 70  # invalid third item is ignored
    assert api.opens == 1 and api.collects == 2
    gpu.close()
    gpu.close()
    assert api.closed == [100]


def test_missing_counter_backoff(monkeypatch):
    gpu, api = make(monkeypatch, fail_add=True)
    assert gpu.percent(100) is None
    assert gpu.percent(103) is None
    assert api.opens == 1 and api.closed == [100]
    assert gpu.percent(161) is None
    assert api.opens == 2


def test_collect_failure_closes_query(monkeypatch):
    gpu, api = make(monkeypatch)
    gpu.percent(100)
    api.fail_collect = True
    assert gpu.percent(103) is None
    assert api.closed == [100]
    assert gpu.percent(106) is None
    assert api.opens == 1


def test_other_platform_no_windows_api(monkeypatch):
    monkeypatch.setattr(wingpu.sys, "platform", "darwin")
    assert wingpu.WindowsGpu().percent(100) is None
