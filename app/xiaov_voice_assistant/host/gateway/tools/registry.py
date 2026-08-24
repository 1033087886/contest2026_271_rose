"""Tool registration, validation, and bounded asynchronous execution.

The schemas intentionally use the OpenAI function-tool shape, but validation
is implemented locally so malformed model output never reaches a handler.
Only the small JSON Schema subset used by this project is supported.
"""

from __future__ import annotations

import asyncio
import copy
import inspect
import json
import math
import re
from collections.abc import Awaitable, Callable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any

ToolHandler = Callable[[dict[str, Any]], Awaitable[Any]]
_TOOL_NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


class ToolRegistryError(ValueError):
    """Raised for an invalid or duplicate tool registration."""


class ToolValidationError(ValueError):
    """Raised when model-generated arguments do not match a tool schema."""


class ToolExecutionError(RuntimeError):
    """A handler error that is safe to return to the model."""

    def __init__(self, message: str, *, code: str = "execution_failed") -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class ToolSpec:
    name: str
    description: str
    parameters: Mapping[str, Any]
    handler: ToolHandler


@dataclass(frozen=True, slots=True)
class ToolResult:
    """JSON-serializable result sent back in an OpenAI ``tool`` message."""

    tool: str
    ok: bool
    result: Any = None
    error_code: str | None = None
    error_message: str | None = None

    def as_dict(self) -> dict[str, Any]:
        if self.ok:
            return {"ok": True, "tool": self.tool, "result": self.result}
        return {
            "ok": False,
            "tool": self.tool,
            "error": {
                "code": self.error_code or "execution_failed",
                "message": self.error_message or "tool execution failed",
            },
        }

    def as_json(self) -> str:
        return json.dumps(
            self.as_dict(), ensure_ascii=False, separators=(",", ":"), sort_keys=True
        )


class ToolRegistry:
    """Holds model-visible tools and executes them behind strict boundaries."""

    def __init__(self, *, max_result_bytes: int = 16_384) -> None:
        if max_result_bytes < 256:
            raise ValueError("max_result_bytes must be at least 256")
        self._tools: dict[str, ToolSpec] = {}
        self.max_result_bytes = max_result_bytes

    def __len__(self) -> int:
        return len(self._tools)

    def register(self, spec: ToolSpec) -> None:
        if not _TOOL_NAME_RE.fullmatch(spec.name):
            raise ToolRegistryError(
                "tool name must contain 1-64 ASCII letters, digits, underscores, or hyphens"
            )
        if spec.name in self._tools:
            raise ToolRegistryError(f"tool already registered: {spec.name}")
        if not spec.description.strip():
            raise ToolRegistryError(f"tool description is empty: {spec.name}")
        if not isinstance(spec.parameters, Mapping):
            raise ToolRegistryError(f"tool parameters must be a schema object: {spec.name}")
        if spec.parameters.get("type") != "object":
            raise ToolRegistryError(f"tool parameters must have type=object: {spec.name}")
        if not callable(spec.handler):
            raise ToolRegistryError(f"tool handler is not callable: {spec.name}")
        self._tools[spec.name] = spec

    def add(
        self,
        *,
        name: str,
        description: str,
        parameters: Mapping[str, Any],
        handler: ToolHandler,
    ) -> None:
        self.register(
            ToolSpec(
                name=name,
                description=description,
                parameters=parameters,
                handler=handler,
            )
        )

    def openai_tools(self) -> list[dict[str, Any]]:
        """Returns defensive copies in OpenAI-compatible request format."""
        return [
            {
                "type": "function",
                "function": {
                    "name": spec.name,
                    "description": spec.description,
                    "parameters": copy.deepcopy(dict(spec.parameters)),
                },
            }
            for spec in self._tools.values()
        ]

    async def execute(
        self,
        name: str,
        arguments: str | Mapping[str, Any],
        *,
        timeout_seconds: float,
    ) -> ToolResult:
        """Validates and executes one call, converting failures to tool results.

        Cancellation from the surrounding request is deliberately not caught.
        Handler exceptions are hidden unless they are explicit ToolExecutionError
        instances, preventing credentials and internal details entering context.
        """
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        spec = self._tools.get(name)
        if spec is None:
            return ToolResult(
                tool=name,
                ok=False,
                error_code="unknown_tool",
                error_message=f"unknown tool: {name}",
            )
        try:
            parsed = _parse_arguments(arguments)
            validate_json(spec.parameters, parsed)
        except ToolValidationError as exc:
            return ToolResult(
                tool=name,
                ok=False,
                error_code="invalid_arguments",
                error_message=str(exc),
            )

        try:
            result = await asyncio.wait_for(
                _invoke(spec.handler, parsed), timeout=timeout_seconds
            )
        except TimeoutError:
            return ToolResult(
                tool=name,
                ok=False,
                error_code="timeout",
                error_message=f"tool exceeded {timeout_seconds:g} second timeout",
            )
        except ToolExecutionError as exc:
            return ToolResult(
                tool=name,
                ok=False,
                error_code=exc.code,
                error_message=str(exc),
            )
        except Exception:
            return ToolResult(
                tool=name,
                ok=False,
                error_code="execution_failed",
                error_message="tool execution failed",
            )

        try:
            encoded = json.dumps(result, ensure_ascii=False, allow_nan=False)
        except (TypeError, ValueError):
            return ToolResult(
                tool=name,
                ok=False,
                error_code="invalid_result",
                error_message="tool returned a non-JSON result",
            )
        if len(encoded.encode("utf-8")) > self.max_result_bytes:
            return ToolResult(
                tool=name,
                ok=False,
                error_code="result_too_large",
                error_message="tool result exceeded the configured size limit",
            )
        return ToolResult(tool=name, ok=True, result=result)


async def _invoke(handler: ToolHandler, arguments: dict[str, Any]) -> Any:
    result = handler(arguments)
    if not inspect.isawaitable(result):
        raise ToolExecutionError("tool handler must be asynchronous")
    return await result


def _parse_arguments(arguments: str | Mapping[str, Any]) -> dict[str, Any]:
    if isinstance(arguments, str):
        try:
            value = json.loads(
                arguments or "{}",
                parse_constant=lambda value: _reject_json_constant(value),
            )
        except json.JSONDecodeError as exc:
            raise ToolValidationError(
                f"arguments are not valid JSON at character {exc.pos}"
            ) from exc
        except (RecursionError, ToolValidationError) as exc:
            raise ToolValidationError("arguments are not valid bounded JSON") from exc
    elif isinstance(arguments, Mapping):
        value = dict(arguments)
    else:
        raise ToolValidationError("arguments must be a JSON object")
    if not isinstance(value, dict):
        raise ToolValidationError("arguments must be a JSON object")
    return value


def _reject_json_constant(value: str) -> None:
    raise ToolValidationError(f"non-standard JSON constant is not allowed: {value}")


def validate_json(schema: Mapping[str, Any], value: Any, path: str = "$") -> None:
    """Validates the intentionally small JSON Schema subset used by tools."""
    if "anyOf" in schema:
        variants = schema["anyOf"]
        if not isinstance(variants, Sequence) or isinstance(variants, (str, bytes)):
            raise ToolRegistryError(f"invalid anyOf schema at {path}")
        for variant in variants:
            try:
                validate_json(variant, value, path)
                break
            except ToolValidationError:
                pass
        else:
            raise ToolValidationError(f"{path} does not match any allowed schema")

    expected = schema.get("type")
    if expected is not None and not _matches_type(expected, value):
        raise ToolValidationError(f"{path} must be {expected}")
    if "const" in schema and value != schema["const"]:
        raise ToolValidationError(f"{path} must equal {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        allowed = ", ".join(repr(item) for item in schema["enum"])
        raise ToolValidationError(f"{path} must be one of: {allowed}")

    if isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", ())
        for key in required:
            if key not in value:
                raise ToolValidationError(f"{path}.{key} is required")
        if schema.get("additionalProperties") is False:
            unknown = sorted(set(value) - set(properties))
            if unknown:
                raise ToolValidationError(
                    f"{path} contains unknown field: {unknown[0]}"
                )
        for key, item in value.items():
            child_schema = properties.get(key)
            if child_schema is not None:
                validate_json(child_schema, item, f"{path}.{key}")

    if isinstance(value, list):
        minimum = schema.get("minItems")
        maximum = schema.get("maxItems")
        if minimum is not None and len(value) < minimum:
            raise ToolValidationError(f"{path} must contain at least {minimum} items")
        if maximum is not None and len(value) > maximum:
            raise ToolValidationError(f"{path} must contain at most {maximum} items")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(value):
                validate_json(item_schema, item, f"{path}[{index}]")

    if isinstance(value, str):
        minimum = schema.get("minLength")
        maximum = schema.get("maxLength")
        if minimum is not None and len(value) < minimum:
            raise ToolValidationError(f"{path} is shorter than {minimum} characters")
        if maximum is not None and len(value) > maximum:
            raise ToolValidationError(f"{path} is longer than {maximum} characters")
        pattern = schema.get("pattern")
        if pattern is not None and re.fullmatch(pattern, value) is None:
            raise ToolValidationError(f"{path} has an invalid format")

    if _is_number(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ToolValidationError(f"{path} must be finite")
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        if minimum is not None and value < minimum:
            raise ToolValidationError(f"{path} must be at least {minimum}")
        if maximum is not None and value > maximum:
            raise ToolValidationError(f"{path} must be at most {maximum}")


def _matches_type(expected: object, value: Any) -> bool:
    if isinstance(expected, list):
        return any(_matches_type(item, value) for item in expected)
    return {
        "object": lambda: isinstance(value, dict),
        "array": lambda: isinstance(value, list),
        "string": lambda: isinstance(value, str),
        "integer": lambda: isinstance(value, int) and not isinstance(value, bool),
        "number": lambda: _is_number(value),
        "boolean": lambda: isinstance(value, bool),
        "null": lambda: value is None,
    }.get(expected, lambda: False)()


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)
