#!/usr/bin/env python3
"""Addestra Qwen3 (licenza Apache 2.0) sui comandi di Kurva3D con LoRA e lo esporta in GGUF.

Serve una GPU NVIDIA (Colab T4 gratuita: 4B con 1200 esempi in circa 1 ora; A100: circa 10 minuti).
Strumenti: unsloth, PEFT, TRL, transformers (Apache 2.0), llama.cpp per il GGUF (MIT).

    pip install unsloth
    python tools/ai-train/generate_dataset.py --count 3000 --out kurva3d-train.jsonl
    python tools/ai-train/train_lora.py --data kurva3d-train.jsonl --base unsloth/Qwen3-4B --out kurva3d-qwen3-4b

Risultato: kurva3d-qwen3-4b/*.gguf (q4_k_m), da provare con
    kurva3d-ai-test --bench --gguf kurva3d-qwen3-4b/<file>.gguf --compact

Lavoro per parti (oggetti complessi: piano + una parte per volta, una conversazione per oggetto):
    python tools/ai-train/generate_parts_dataset.py --count 400 --out kurva3d-parts.jsonl
    python tools/ai-train/train_lora.py --parts --data kurva3d-parts.jsonl --out kurva3d-qwen3-4b-parti
    kurva3d-ai-test --bench-complex --gguf kurva3d-qwen3-4b-parti/<file>.gguf --compact
"""
import argparse
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--base", default="unsloth/Qwen3-4B")
    ap.add_argument("--out", default="kurva3d-qwen3-4b")
    # ogni esempio porta con se' le istruzioni (~1600 token): su una T4 gratuita
    # 1200 esempi per 1 giro sono circa un'ora; 3000 x 2 giri sarebbero oltre 6 ore
    ap.add_argument("--epochs", type=float, default=1)
    ap.add_argument("--max-examples", type=int, default=1200)
    ap.add_argument("--rank", type=int, default=16)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--max-len", type=int, default=4096)
    ap.add_argument("--quant", default="q4_k_m")
    ap.add_argument("--eval-share", type=float, default=0.03)
    # esempi a parti: conversazioni lunghe (istruzioni una volta, piano, poi tutte le parti)
    ap.add_argument("--parts", action="store_true")
    a = ap.parse_args()
    if a.parts and a.max_len == 4096:
        a.max_len = 8192
    batch, accum = (1, 16) if a.parts else (2, 8)

    from unsloth import FastLanguageModel  # importato prima di transformers, come chiede unsloth
    from datasets import Dataset
    from trl import SFTConfig, SFTTrainer

    rows = [json.loads(l) for l in open(a.data, encoding="utf-8") if l.strip()]
    random.Random(1).shuffle(rows)
    rows = rows[:a.max_examples]
    n_eval = max(1, int(len(rows) * a.eval_share))

    model, tok = FastLanguageModel.from_pretrained(a.base, max_seq_length=a.max_len, load_in_4bit=True)
    model = FastLanguageModel.get_peft_model(
        model, r=a.rank, lora_alpha=a.rank * 2, lora_dropout=0,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
        use_gradient_checkpointing="unsloth", random_state=1)

    def text(r):
        # Kurva3D chiede le risposte senza ragionamento lungo: niente blocco <think>
        return tok.apply_chat_template(r["messages"], tokenize=False, enable_thinking=False)

    train = Dataset.from_list([{"text": text(r)} for r in rows[n_eval:]])
    evals = Dataset.from_list([{"text": text(r)} for r in rows[:n_eval]])
    trainer = SFTTrainer(
        model=model, tokenizer=tok, train_dataset=train, eval_dataset=evals,
        args=SFTConfig(dataset_text_field="text", max_seq_length=a.max_len, per_device_train_batch_size=batch,
                       gradient_accumulation_steps=accum, num_train_epochs=a.epochs, learning_rate=a.lr,
                       lr_scheduler_type="cosine", warmup_steps=5, logging_steps=10, eval_strategy="epoch",
                       optim="adamw_8bit", seed=1, output_dir=a.out + "-run", report_to="none"))
    try:  # impara solo le risposte, non le istruzioni ripetute in ogni esempio
        from unsloth.chat_templates import train_on_responses_only
        trainer = train_on_responses_only(trainer, instruction_part="<|im_start|>user\n",
                                          response_part="<|im_start|>assistant\n")
    except Exception as e:  # versioni vecchie di unsloth
        print("train_on_responses_only non disponibile:", e)
    if a.parts:  # una conversazione troppo lunga verrebbe tagliata a meta': meglio saperlo
        lens = [len(tok(t)["input_ids"]) for t in train["text"]]
        print("token per oggetto: max %d, media %d, oltre %d: %d" % (
            max(lens), sum(lens) // len(lens), a.max_len, sum(1 for x in lens if x > a.max_len)))
    trainer.train()
    print(trainer.evaluate())

    # prova veloce prima dell'esportazione
    FastLanguageModel.for_inference(model)
    sys_msg = rows[0]["messages"][0]
    tests = ["un tavolo con gamba centrale", "una mensola lunga 80 cm con due staffe sotto"]
    if a.parts:  # primo passo del lavoro per parti: il piano (stesso testo di ai::PlanRequest compatto)
        from generate_parts_dataset import plan_request
        tests = [plan_request(q) for q in ["un violino", "un monopattino elettrico", "una sedia a dondolo"]]
    for q in tests:
        ids = tok.apply_chat_template([sys_msg, {"role": "user", "content": q}], add_generation_prompt=True,
                                      enable_thinking=False, return_tensors="pt").to(model.device)
        out = model.generate(ids, max_new_tokens=600, do_sample=False)
        print("==", q, "\n" + tok.decode(out[0][ids.shape[1]:], skip_special_tokens=True))

    model.save_pretrained(a.out + "-lora")
    tok.save_pretrained(a.out + "-lora")
    model.save_pretrained_gguf(a.out, tok, quantization_method=a.quant)
    print("GGUF salvato in", a.out)


if __name__ == "__main__":
    main()
