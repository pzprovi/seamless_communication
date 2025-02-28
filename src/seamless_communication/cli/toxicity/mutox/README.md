# MuTox: MuTox: Universal MUltilingual Audio-based TOXicity Dataset and Zero-shot Detector

MuTox, the first highly multilingual audio-based dataset with toxicity labels.
The dataset consists of 20k audio utterances for English and Spanish, and 4k for
the other 19 languages. To showcase the quality of this dataset, we train the
MuTox audio-based toxicity classifier, which allows zero-shot toxicity detection
across a broad range of languages. This classifier outperforms existing
text-based trainable classifiers by more than 1% AUC, while increasing the
language coverage from 8 to 100+ languages. When compared to a wordlist-based
classifier that covers a similar number of languages, MuTox improves precision
and recall by ∼2.5 times.

## License

The mutox code and model are licensed under the MIT license (see MIT_LICENSE
file at the root of seamless_communication). The mutox model depends on SONAR
encoders, most are under the MIT license but a few are under CC-BY-NC license.
See the [SONAR repository](https://github.com/facebookresearch/SONAR) for
details.

## Dataset Languages.

- English,
- Spanish,
- Arabic,
- Bengali,
- Mandarin Chinese,
- Dutch,
- French,
- German,
- Hindi,
- Indonesian,
- Italian,
- Japanese,
- Korean,
- Portuguese,
- Russian,
- Swahili,
- Tagalog,
- Thai,
- Turkish,
- Urdu,
- Vietnamese

## Classifier details.

We use multi-modal and multilingual
[SONAR](https://github.com/facebookresearch/SONAR) encoders from (Duquenne et
al., 2023). For the classifier, we use variable input sizes for the 3
feedforward layers (1024, 512, and 128).

## Classifier Quick Start

This introduces the MuTox speech toxicity model, this relies on computing the
sonar embedding and then classifying it through the MuTox model. The
`cli/mutox/mutox.py` provides an example of reading a TSV, computing the SONAR
embedding and running the classifier on the results:

```bash
python -m seamless_communication.cli.toxicity.mutox.mutox_speech --lang fra --audio_column ref_tgt_audio /checkpoint/bokai/seamless/toxity_mitigation/exps_v5/joined_etox/fleurs/s2t/en-xx/fra.tsv /tmp/tesmortt.tsv
```

You can also work with text:

```bash
python -m seamless_communication.cli.toxicity.mutox.mutox_text --lang fra_Latn sentences.txt
```

You can also check the mutox example notebook in this directory.

## Dataset

The dataset is available in this [file](https://dl.fbaipublicfiles.com/seamless/datasets/mutox.csv). The dataset is licensed under the MIT license (see MIT_LICENSE
file at the root of seamless_communication).

## Citation

```bitex
@misc{costajussà2023mutox,
      title={MuTox: Universal MUltilingual Audio-based TOXicity Dataset and Zero-shot Detector},
      author={ Marta R. Costa-jussà, Mariano Coria Meglioli, Pierre Andrews, David Dale, Prangthip Hansanti, Elahe Kalbassi, Alex Mourachko, Christophe Ropers, Carleigh Wood},
      year={2023},
      eprint={},
      archivePrefix={arXiv},
      primaryClass={cs.CL}
}
```
