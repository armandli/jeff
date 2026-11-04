from jeff.data.loader import DecisionExample


def format_dpo_example(example: DecisionExample) -> dict:
    """Convert a labelled decision example to a DPO (chosen/rejected) pair."""
    opts = "\n".join(f"{i}. {o}" for i, o in enumerate(example.options))
    prompt = f"{example.context}\n\nOptions:\n{opts}\n\nAnswer: "
    chosen = str(example.correct_index)
    # Use a random incorrect option as rejected; caller may want to vary this.
    rejected_index = (example.correct_index + 1) % len(example.options)
    rejected = str(rejected_index)
    return {"prompt": prompt, "chosen": chosen, "rejected": rejected}


def format_sft_example(example: DecisionExample) -> dict:
    """Convert to a simple supervised fine-tuning example."""
    opts = "\n".join(f"{i}. {o}" for i, o in enumerate(example.options))
    prompt = f"{example.context}\n\nOptions:\n{opts}\n\nAnswer: "
    return {"prompt": prompt, "completion": str(example.correct_index)}
