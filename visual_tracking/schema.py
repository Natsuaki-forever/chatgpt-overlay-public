"""Configuration-only validation; imports no GUI or numerical dependencies."""
import math
import re
from copy import deepcopy


def _number(value, low, high):
    if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError('invalid_visual_number')


def _array(values, count, low, high):
    if not isinstance(values, list) or len(values) != count:
        raise ValueError('invalid_visual_array')
    for value in values:
        _number(value, low, high)


def _keys(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys.split()):
        raise ValueError('invalid_visual_fields')


def binding_copy(binding):
    if binding is None:
        return None
    if not isinstance(binding,dict):
        raise ValueError('invalid_visual_fields')
    _keys(binding, 'version calibration_id reference_rect_dip calibration_dpi template_id template_sha256 edge_limit_dip size_ratio relation'+(' tracking_kind' if 'tracking_kind' in binding else '')+(' search_limit_dip' if 'search_limit_dip' in binding else ''))
    kind=binding.get('tracking_kind','boundary')
    if kind not in ('boundary','features'):
        raise ValueError('invalid_tracking_kind')
    if 'search_limit_dip' in binding:_number(binding['search_limit_dip'],32,500)
    if type(binding['version']) is not int or binding['version'] != 1:
        raise ValueError('invalid_visual_identity')
    for key, length in (('calibration_id', 32), ('template_id', 32), ('template_sha256', 64)):
        if not isinstance(binding[key], str) or not re.fullmatch('[0-9a-fA-F]{'+str(length)+'}', binding[key]):
            raise ValueError('invalid_visual_identity')
    _array(binding['reference_rect_dip'], 4, 0, 100000)
    if min(binding['reference_rect_dip'][2:]) <= 0:
        raise ValueError('invalid_visual_reference')
    _number(binding['calibration_dpi'], 48, 768)
    _number(binding['edge_limit_dip'], 32, 32)
    _number(binding['size_ratio'], 1.25, 1.25)
    relation = binding['relation']
    _keys(relation, 'mode anchor_x anchor_y offset_dip fixed_size_dip margins_dip padding_physical')
    if relation['mode'] not in ('position_only', 'follow_bounds'):
        raise ValueError('invalid_visual_relation')
    if kind=='features' and relation['mode']!='position_only':
        raise ValueError('feature_position_requires_fixed_size')
    if relation['anchor_x'] not in ('left', 'center', 'right') or relation['anchor_y'] not in ('top', 'center', 'bottom'):
        raise ValueError('invalid_visual_anchor')
    _array(relation['offset_dip'], 2, -100000, 100000)
    _array(relation['fixed_size_dip'], 2, 8, 100000)
    _array(relation['margins_dip'], 4, -4096, 4096)
    if type(relation['padding_physical']) is not int:
        raise ValueError('invalid_visual_padding')
    _number(relation['padding_physical'], 0, 2)
    return deepcopy(binding)
