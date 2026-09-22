class JevError(Exception):
    """Public, credential-free error safe to surface over MCP."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code

    def as_dict(self) -> dict:
        return {"ok": False, "error": {"code": self.code, "message": str(self)}}
