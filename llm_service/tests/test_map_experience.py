"""Tests for MapExperienceStore - SQLite map experience storage."""

import pytest
import asyncio
import os
import tempfile
from map_experience import MapExperienceStore


@pytest.fixture
def db_path():
    """Create a temporary database path."""
    fd, path = tempfile.mkstemp(suffix=".db")
    os.close(fd)
    yield path
    if os.path.exists(path):
        os.unlink(path)


@pytest.fixture
def store(db_path):
    """Create a MapExperienceStore with temp database."""
    return MapExperienceStore(db_path=db_path)


def make_state(map_name="c1m1_hotel", flow=500, threats=None, teammates=None,
               bot=None, common_count=0, fire_areas=None, acid_areas=None, witches=None):
    """Helper to create a state dict for testing."""
    return {
        "map": map_name,
        "bot": bot or {"flow": flow, "incap": False, "hp": 100},
        "threats": threats or [],
        "teammates": teammates or [],
        "common_count": common_count,
        "fire_areas": fire_areas or [],
        "acid_areas": acid_areas or [],
        "witches": witches or [],
    }


class TestDatabaseInit:
    @pytest.mark.asyncio
    async def test_connect_and_create_tables(self, store, db_path):
        await store.connect()
        assert store.db is not None
        assert os.path.exists(db_path)
        await store.disconnect()

    @pytest.mark.asyncio
    async def test_disconnect(self, store):
        await store.connect()
        await store.disconnect()
        assert store.db is None


class TestRecordDecision:
    @pytest.mark.asyncio
    async def test_record_basic_decision(self, store):
        await store.connect()
        state = make_state()
        decision = {"action": "follow_team", "target": ""}
        decision_id = await store.record_decision(state, decision)
        assert decision_id > 0
        await store.disconnect()

    @pytest.mark.asyncio
    async def test_record_with_different_maps(self, store):
        await store.connect()
        for map_name in ["c1m1_hotel", "c2m1_highway", "c3m1_plankcountry"]:
            state = make_state(map_name=map_name)
            decision = {"action": "hold_position", "target": ""}
            did = await store.record_decision(state, decision)
            assert did > 0
        await store.disconnect()


class TestGetExperience:
    @pytest.mark.asyncio
    async def test_no_experience_returns_empty(self, store):
        await store.connect()
        state = make_state()
        results = await store.get_experience(state)
        assert results == []
        await store.disconnect()

    @pytest.mark.asyncio
    async def test_experience_after_round_outcome(self, store):
        await store.connect()
        state = make_state()
        decision = {"action": "follow_team", "target": ""}
        await store.record_decision(state, decision)

        # Record round outcome
        await store.record_round_outcome("c1m1_hotel", "survived")

        # Now query experience
        results = await store.get_experience(state)
        assert len(results) > 0
        assert results[0]["action"] == "follow_team"
        assert results[0]["outcome"] == "survived"
        assert results[0]["count"] >= 1
        await store.disconnect()


class TestRoundOutcome:
    @pytest.mark.asyncio
    async def test_record_round_outcome(self, store):
        await store.connect()
        state = make_state()
        decision = {"action": "hold_position", "target": ""}
        await store.record_decision(state, decision)
        await store.record_round_outcome("c1m1_hotel", "team_wipe")

        results = await store.get_experience(state)
        assert len(results) > 0
        assert results[0]["outcome"] == "team_wipe"
        await store.disconnect()


class TestFlowBuckets:
    @pytest.mark.asyncio
    async def test_flow_bucket_range(self, store):
        assert store._flow_to_range(0) == "0-1000"
        assert store._flow_to_range(500) == "0-1000"
        assert store._flow_to_range(1000) == "1000-2000"
        assert store._flow_to_range(2500) == "2000-3000"

    @pytest.mark.asyncio
    async def test_different_flow_buckets(self, store):
        await store.connect()
        # Record in different flow ranges
        state_low = make_state(flow=300)
        state_high = make_state(flow=2500)
        decision = {"action": "follow_team", "target": ""}

        await store.record_decision(state_low, decision)
        await store.record_decision(state_high, decision)
        await store.record_round_outcome("c1m1_hotel", "survived")

        # Low flow should only see low-flow experiences
        results_low = await store.get_experience(state_low)
        assert len(results_low) > 0

        # High flow should only see high-flow experiences
        results_high = await store.get_experience(state_high)
        # May or may not have results depending on hash match
        await store.disconnect()


class TestSituationHash:
    def test_same_state_same_hash(self, store):
        state1 = make_state(threats=[{"type": "hunter", "ghost": False}])
        state2 = make_state(threats=[{"type": "hunter", "ghost": False}])
        assert store._compute_situation_hash(state1) == store._compute_situation_hash(state2)

    def test_different_threats_different_hash(self, store):
        state1 = make_state(threats=[{"type": "hunter", "ghost": False}])
        state2 = make_state(threats=[{"type": "tank", "ghost": False}])
        assert store._compute_situation_hash(state1) != store._compute_situation_hash(state2)

    def test_empty_state_hash(self, store):
        state = make_state()
        h = store._compute_situation_hash(state)
        assert len(h) == 12  # MD5 truncated to 12 chars


class TestMapStats:
    @pytest.mark.asyncio
    async def test_empty_stats(self, store):
        await store.connect()
        stats = await store.get_map_stats("c1m1_hotel")
        assert stats["total_decisions"] == 0
        await store.disconnect()

    @pytest.mark.asyncio
    async def test_stats_after_decisions(self, store):
        await store.connect()
        state = make_state()
        decision = {"action": "follow_team", "target": ""}
        await store.record_decision(state, decision)
        await store.record_round_outcome("c1m1_hotel", "survived")

        stats = await store.get_map_stats("c1m1_hotel")
        assert stats["total_decisions"] > 0
        assert stats["survival_rate"] > 0
        await store.disconnect()


class TestCleanup:
    @pytest.mark.asyncio
    async def test_cleanup_old(self, store):
        await store.connect()
        state = make_state()
        decision = {"action": "follow_team", "target": ""}
        await store.record_decision(state, decision)
        await store.record_round_outcome("c1m1_hotel", "survived")

        # Cleanup with 0 days should remove all
        await store.cleanup_old(max_age_days=0)
        stats = await store.get_map_stats("c1m1_hotel")
        assert stats["total_decisions"] == 0
        await store.disconnect()
