"""Render frozen scalar observations with explicit public null predicates."""
import math


def render(observation: dict) -> str:
    value = observation['expected']
    projection = observation.get('projection')
    if projection == 'is_null':
        if value is not None:
            raise ValueError('null presence projection requires a frozen absent value')
        return 'true'
    if projection == 'optional_payload':
        if value is None:
            raise ValueError('payload observation requires a frozen present value')
        projection = None
    if projection is not None:
        raise ValueError('unknown semantic observation projection')
    if type(value) is bool:
        return str(value).lower()
    if type(value) in (int, str):
        return str(value)
    if type(value) is float and math.isfinite(value):
        text = repr(value)
        if 'e' in text:
            coefficient, exponent = text.split('e')
            return coefficient + 'e' + ('+' if int(exponent) >= 0 else '-') + str(abs(int(exponent)))
        return text
    raise ValueError('an explicit scalar or absence predicate is required')
