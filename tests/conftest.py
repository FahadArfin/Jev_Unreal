"""Provider fixtures contain synthetic data only; tests never call a live model."""

import copy

import httpx
import pytest
import pytest_asyncio

from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient


@pytest.fixture
def decision_questions():
    return {
        "route": {
            "type": "choice",
            "instructions": "Which listed operation matches the user request?",
            "criteria": {
                "list_assets": "Find existing project assets",
                "review": "No listed operation fits, or more information is needed",
            },
        }
    }


@pytest.fixture
def decision_response():
    return {
        "model": "typesafe/jev-1.13",
        "answers": {
            "route": {
                "type": "choice",
                "choice": "list_assets",
                "confidence": 0.9,
                "probabilities": {"list_assets": 0.96, "review": 0.04},
            }
        },
        "usage": {"input_tokens": 120, "output_tokens": 20, "cost": 0.00000504},
    }


@pytest_asyncio.fixture
async def decision_client_factory(decision_response):
    clients = []

    def make(handler=None, **overrides):
        if handler is None:

            def handler(request):
                return httpx.Response(200, json=copy.deepcopy(decision_response))

        settings = Settings(**{"api_key": "synthetic-test-key", **overrides})
        client = DecisionClient(settings, transport=httpx.MockTransport(handler))
        clients.append(client)
        return client

    yield make
    for client in clients:
        await client.close()
