"""Identity teaching model. It does NOT emulate RTOS locks or resource freeing."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Token:
    epoch: int
    request_id: str


class Owner:
    def __init__(self):
        self.epoch = 0
        self.active = None

    def reconnect(self):
        # A real implementation must first cancel/join old resource users.
        self.epoch += 1
        self.active = None

    def begin(self, request_id):
        if self.active is not None:
            raise RuntimeError("owner busy")
        self.active = Token(self.epoch, request_id)
        return self.active

    def complete(self, token):
        if token != self.active or token.epoch != self.epoch:
            return False
        self.active = None
        return True


def self_test():
    owner = Owner()
    owner.reconnect()
    old = owner.begin("request_A")
    owner.reconnect()
    current = owner.begin("request_B")
    assert not owner.complete(old)
    assert owner.active == current
    assert not owner.complete(Token(current.epoch, "wrong_request"))
    assert owner.complete(current)
    assert not owner.complete(current)  # Duplicate callback has no second effect.
    return "owner_epoch: PASS (old/incorrect/duplicate completion rejected)"


if __name__ == "__main__":
    print(self_test())
