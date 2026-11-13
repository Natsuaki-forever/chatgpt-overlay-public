"""Bounded snapshot assessment, extracted from region-follow-lab.

No simulator, UIA, target process memory or Qt access. This is not a scheduler.
"""
import cv2
import numpy as np

from .contracts import LIMITS, checked_dpi, integer_rect

ALGORITHM_REVISION = 'roi-rounded-2'


def gray(image):
    if (not isinstance(image, np.ndarray) or image.dtype != np.uint8 or
            image.ndim not in (2, 3) or image.size == 0):
        raise ValueError('invalid-image')
    if image.shape[0]*image.shape[1] > LIMITS.roi_pixels:
        raise ValueError('roi-pixel-budget-exceeded')
    if image.ndim == 2:
        return image
    if image.shape[2] != 3:
        raise ValueError('expected-bgr-image')
    return cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)


def learn(image, rect, tracking_kind='boundary'):
    frame = gray(image)
    x, y, w, h = integer_rect(rect, frame.shape[1], frame.shape[0])
    if tracking_kind == 'features':
        # Keep the user's patch coordinates intact. Text and symbols need no
        # enclosing border, inset or inferred rectangle.
        if w*h > LIMITS.template_pixels:
            raise ValueError('template-pixel-budget-exceeded')
        template = frame[y:y+h, x:x+w].copy()
        if float(np.std(template)) < 4 or np.count_nonzero(cv2.Canny(template, 30, 90)) < 12:
            raise ValueError('insufficient-stable-features')
        return template
    if tracking_kind != 'boundary':
        raise ValueError('invalid-tracking-kind')
    crop = frame[y+5:y+h-5, x+5:x+w-5]
    # Estimate a flat background from the inner perimeter, away from the border.
    perimeter = np.concatenate((crop[:3].ravel(), crop[-3:].ravel(),
                                crop[:, :3].ravel(), crop[:, -3:].ravel()))
    background = float(np.median(perimeter))
    if np.percentile(np.abs(perimeter.astype(np.float32)-background), 90) > 12:
        raise ValueError('background-not-stable')
    delta = np.abs(crop.astype(np.float32)-background)
    threshold = max(24., float(np.percentile(np.abs(perimeter.astype(np.float32)-background), 90))*3)
    mask = (delta >= threshold).astype(np.uint8)
    # Rounded borders can leave disconnected high-contrast fragments inside
    # the five-pixel inset. Do not learn those corners as stable text features.
    corner = max(1, int(min(crop.shape)*.35))
    mask[:corner, :corner] = mask[:corner, -corner:] = 0
    mask[-corner:, :corner] = mask[-corner:, -corner:] = 0
    if np.mean(mask) > .3:
        raise ValueError('features-too-dense')
    # Reject frame lines and large fills instead of learning an empty background.
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    if count > 512:
        raise ValueError('features-too-complex')
    selected = np.zeros(mask.shape, np.uint8)
    for i in range(1, count):
        a, b, cw, ch, area = stats[i]
        if (area < 2 or area > crop.size*.15 or a == 0 or b == 0 or
                a+cw == crop.shape[1] or b+ch == crop.shape[0] or
                cw > crop.shape[1]*.8 or ch > crop.shape[0]*.8):
            continue
        selected[labels == i] = 1
    yy, xx = np.where(selected)
    if len(xx) < 25 or len(xx) > crop.size*.3:
        raise ValueError('insufficient-stable-features')
    a, b = max(0, xx.min()-3), max(0, yy.min()-3)
    c, d = min(crop.shape[1], xx.max()+4), min(crop.shape[0], yy.max()+4)
    template = crop[b:d, a:c].copy()
    if template.size > LIMITS.template_pixels:
        raise ValueError('template-pixel-budget-exceeded')
    if float(np.std(template)) < 4:
        raise ValueError('template-low-variance')
    return template


def same_border_stroke(frame, first, second):
    # A 2-DIP stroke at fractional Windows DPI can produce inner/outer Canny
    # edges four pixels apart. Merge only a continuous contrasting stripe;
    # two separate nested outlines retain their background gap and ambiguity.
    ax, ay, aw, ah = first
    bx, by, bw, bh = second
    if max(abs(ax-bx), abs(ay-by), abs(ax+aw-bx-bw), abs(ay+ah-by-bh)) > 6:
        return False
    left, top = max(ax, bx), max(ay, by)
    right, bottom = min(ax+aw, bx+bw), min(ay+ah, by+bh)
    if right-left < 20 or bottom-top < 20:
        return False
    background = float(np.median(frame[top+5:bottom-5, left+5:right-5]))
    inset = max(5, int(min(right-left, bottom-top)*.35))
    bands = (frame[top+inset:bottom-inset, min(ax,bx)+1:max(ax,bx)+1],
             frame[top+inset:bottom-inset, min(ax+aw,bx+bw):max(ax+aw,bx+bw)],
             frame[min(ay,by)+1:max(ay,by)+1, left+inset:right-inset],
             frame[min(ay+ah,by+bh):max(ay+ah,by+bh), left+inset:right-inset])
    return all(not b.size or np.mean(np.abs(b.astype(np.float32)-background)>16) >= .95 for b in bands)


def boundaries(frame):
    boxes = []
    # Audit both contrast levels so a strong inner border cannot hide an
    # ambiguous weaker outer border. No gap-closing morphology invents a side.
    for low, high in ((45, 120), (12, 36)):
        edges = cv2.Canny(frame, low, high)
        contours, _ = cv2.findContours(edges, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
        if len(contours) > 4096:
            raise ValueError('boundaries-too-complex')
        for contour in contours:
            x, y, w, h = cv2.boundingRect(contour)
            if w < 140 or h < 55 or w*h > frame.size*.999:
                continue
            # Closed broad enclosure, rather than the bounding box of a U-shape.
            if abs(cv2.contourArea(contour)) < w*h*.75:
                continue
            # Ignore corner arcs, but require independent straight support on
            # every central side. Circles/diamonds/open boxes must not qualify.
            inset = max(3, int(min(w, h)*.3))
            bands = (edges[y:y+3, x+inset:x+w-inset],
                     edges[y+h-3:y+h, x+inset:x+w-inset],
                     edges[y+inset:y+h-inset, x:x+3].T,
                     edges[y+inset:y+h-inset, x+w-3:x+w].T)
            if min(np.mean(np.any(b > 0, axis=0)) for b in bands) < .86:
                continue
            longest_gaps = []
            for band in bands:
                support = np.any(band > 0, axis=0)
                positions = np.concatenate(([-1], np.flatnonzero(support), [len(support)]))
                longest_gaps.append(int(np.max(np.diff(positions))-1))
            if max(longest_gaps) > 3:
                continue
            duplicate = next((i for i, (a, b, cw, ch) in enumerate(boxes)
                              if max(abs(x-a), abs(y-b), abs(x+w-a-cw), abs(y+h-b-ch)) <= 3 or
                              same_border_stroke(frame, (x,y,w,h), (a,b,cw,ch))), None)
            if duplicate is None:
                boxes.append((x, y, w, h))
            elif w*h > boxes[duplicate][2]*boxes[duplicate][3]:
                boxes[duplicate] = (x, y, w, h)
    return boxes


def locate(image, template, tracking_kind='boundary'):
    frame = gray(image)
    if template.ndim != 2 or template.size > LIMITS.template_pixels or np.std(template) < 4:
        raise ValueError('invalid-template')
    th, tw = template.shape
    if th > frame.shape[0] or tw > frame.shape[1]:
        return None, 'template-too-large', 0.
    scores = cv2.matchTemplate(frame, template, cv2.TM_CCOEFF_NORMED)
    _, best, _, (tx, ty) = cv2.minMaxLoc(scores)
    if not np.isfinite(best) or best < .84:
        return None, 'features-lost', float(best) if np.isfinite(best) else 0.
    scores[max(0, ty-th//2):ty+th//2+1, max(0, tx-tw//2):tx+tw//2+1] = -1
    second = cv2.minMaxLoc(scores)[1]
    if second >= .84 or best-second < .08:
        return None, 'ambiguous-features', float(best)
    if tracking_kind == 'features':
        return (tx, ty, tw, th), 'unique-feature-position', float(best)
    if tracking_kind != 'boundary':
        raise ValueError('invalid-tracking-kind')
    boxes = [b for b in boundaries(frame) if b[0] <= tx and b[1] <= ty and
             b[0]+b[2] >= tx+tw and b[1]+b[3] >= ty+th]
    if len(boxes) != 1:
        return None, 'boundary-unobservable-or-ambiguous', float(best)
    x, y, w, h = boxes[0]
    # Preserve the experiment's boundary estimate; cover padding is separate.
    x0, y0 = max(0, x-2), max(0, y-2)
    return (x0, y0, min(frame.shape[1], x+w+2)-x0,
            min(frame.shape[0], y+h+2)-y0), 'snapshot-supported', float(best)


def assess(image, selection, dpi=96):
    dpi = checked_dpi(dpi)
    frame = gray(image)
    x, y, w, h = integer_rect(selection, frame.shape[1], frame.shape[0])
    template = learn(frame, (x, y, w, h))
    pad = int(np.ceil(LIMITS.edge_dip*dpi/96))
    x0, y0 = max(0, x-pad), max(0, y-pad)
    x1, y1 = min(frame.shape[1], x+w+pad+1), min(frame.shape[0], y+h+pad+1)
    rect, status, score = locate(frame[y0:y1, x0:x1], template)
    if rect:
        rect = (rect[0]+x0, rect[1]+y0, rect[2], rect[3])
        reference_edges = (x, y, x+w, y+h)
        found_edges = (rect[0], rect[1], rect[0]+rect[2], rect[1]+rect[3])
        if max(abs(a-b)*96/dpi for a, b in zip(reference_edges, found_edges)) > LIMITS.edge_dip:
            rect, status = None, 'outside-authorized-reference'
        elif any(not 1/LIMITS.size_ratio <= rect[i]/selection[i] <= LIMITS.size_ratio for i in (2, 3)):
            rect, status = None, 'outside-authorized-size'
    return dict(supported=rect is not None, status=status, boundary_rect=rect,
                algorithm_revision=ALGORITHM_REVISION,
                score=score, dpi=dpi, template_size=list(template.shape[::-1]),
                authorized_rect=[x0, y0, x1-x0, y1-y0],
                matching_method='contrast-selected-grayscale-patch', matching_masked=False,
                corner_feature_exclusion_fraction=.35,
                evidence='single-snapshot-only', live_tracking_verified=False)
