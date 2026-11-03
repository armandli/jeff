from dataclasses import dataclass, field
from pathlib import Path

from transformers import TrainingArguments
from trl import SFTTrainer


@dataclass
class JeffTrainingConfig:
    model_name: str = "Qwen/Qwen2.5-7B-Instruct"
    output_dir: str = "checkpoints/exp"
    num_train_epochs: int = 3
    per_device_train_batch_size: int = 4
    gradient_accumulation_steps: int = 8
    learning_rate: float = 2e-5
    warmup_ratio: float = 0.05
    bf16: bool = True
    logging_steps: int = 10
    save_steps: int = 200
    eval_steps: int = 200
    lora_r: int = 64
    lora_alpha: int = 128
    lora_target_modules: list[str] = field(default_factory=lambda: ["q_proj", "v_proj"])


class JeffTrainer:
    """SFT wrapper for jeff — fine-tunes Qwen 2.5 on labelled decision data."""

    def __init__(self, config: JeffTrainingConfig):
        self.config = config

    def train(self, train_dataset, eval_dataset=None) -> None:
        from peft import LoraConfig, get_peft_model
        from transformers import AutoModelForCausalLM, AutoTokenizer

        cfg = self.config
        tokenizer = AutoTokenizer.from_pretrained(cfg.model_name)
        model = AutoModelForCausalLM.from_pretrained(cfg.model_name, torch_dtype="auto")

        peft_cfg = LoraConfig(
            r=cfg.lora_r,
            lora_alpha=cfg.lora_alpha,
            target_modules=cfg.lora_target_modules,
            bias="none",
            task_type="CAUSAL_LM",
        )
        model = get_peft_model(model, peft_cfg)

        training_args = TrainingArguments(
            output_dir=cfg.output_dir,
            num_train_epochs=cfg.num_train_epochs,
            per_device_train_batch_size=cfg.per_device_train_batch_size,
            gradient_accumulation_steps=cfg.gradient_accumulation_steps,
            learning_rate=cfg.learning_rate,
            warmup_ratio=cfg.warmup_ratio,
            bf16=cfg.bf16,
            logging_steps=cfg.logging_steps,
            save_steps=cfg.save_steps,
            eval_steps=cfg.eval_steps,
        )

        trainer = SFTTrainer(
            model=model,
            tokenizer=tokenizer,
            train_dataset=train_dataset,
            eval_dataset=eval_dataset,
            args=training_args,
        )
        trainer.train()
        Path(cfg.output_dir).mkdir(parents=True, exist_ok=True)
        trainer.save_model()
