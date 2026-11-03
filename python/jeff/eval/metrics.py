import math


def accuracy(predictions: list[int], labels: list[int]) -> float:
    if not predictions:
        return 0.0
    return sum(p == l for p, l in zip(predictions, labels)) / len(predictions)


def ece(scores_list: list[list[float]], labels: list[int], n_bins: int = 10) -> float:
    """Expected Calibration Error — lower is better calibrated."""
    bins = [[] for _ in range(n_bins)]
    for scores, label in zip(scores_list, labels):
        # Softmax to get probabilities from raw log-probs
        max_s = max(scores)
        exps = [math.exp(s - max_s) for s in scores]
        total = sum(exps)
        probs = [e / total for e in exps]
        conf = max(probs)
        correct = int(probs.index(max(probs)) == label)
        bin_idx = min(int(conf * n_bins), n_bins - 1)
        bins[bin_idx].append((conf, correct))

    ece_val = 0.0
    n = len(labels)
    for b in bins:
        if not b:
            continue
        avg_conf = sum(x[0] for x in b) / len(b)
        avg_acc = sum(x[1] for x in b) / len(b)
        ece_val += (len(b) / n) * abs(avg_conf - avg_acc)
    return ece_val
