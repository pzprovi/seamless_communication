#include <math.h>
#include "ggml.h"
#include "fairseq2.h"
#include <unordered_map>
#include <algorithm>


/// allocate the fairseq2 model and hyperparameters
extern "C" fairseq2_model* fairseq2_model_alloc() {
    // pre-allocate some memory to write hyperparameters and tensors pointers
    auto* model = new fairseq2_model;
    model->hparams = new std::uint8_t[8 * 1024];
    model->arch = new std::uint64_t[16 * 1024];  // max tensors allowed
    model->tensors_ctx = nullptr;
    return model;
};

extern "C" void fairseq2_model_free(fairseq2_model* model) {
    if (model->tensors_ctx) ggml_free(model->tensors_ctx);
    delete (std::uint64_t*)(model->arch);
    delete (std::uint8_t*)model->hparams;
    delete model;
};

extern "C" void fairseq2_model_set_inference_ctx(fairseq2_model* model, ggml_context* ctx) {
    model->ctx = ctx;
}

extern "C" std::string* std_string_alloc(char* c_str) {
    return new std::string(c_str);
}

extern "C" void std_string_free(std::string* str) {
    delete str;
}

bool has_layer(fairseq2_model& model, const std::string& name) {
    return model.tensors.find(name) != model.tensors.end();
}

extern "C" ggml_tensor* Linear_forward(
    fairseq2_model& model,
    const std::string &prefix,
    ggml_tensor* input  // (d_in)
) {
    // Note: for now we assumed un-batched input
    ggml_tensor* weight = model.tensors[prefix + ".weight"];  // (d_in, d_out)
    GGML_ASSERT(weight != nullptr);
    ggml_tensor* out = ggml_mul_mat(model.ctx, weight, input);  // (d_out)

    ggml_tensor* bias = model.tensors[prefix + ".bias"];  // (d_out)
    if (bias == nullptr) return out;

    return ggml_add_inplace(model.ctx, out, bias);
}

extern "C" ggml_tensor* LayerNorm_forward(
    fairseq2_model& model,
    const std::string &prefix,
    ggml_tensor* input
) {
    ggml_tensor* weight = model.tensors[prefix + ".weight"];
    GGML_ASSERT(weight != nullptr);
    ggml_tensor* bias = model.tensors[prefix + ".bias"];
    GGML_ASSERT(bias != nullptr);

    auto ctx = model.ctx;
    // TODO: should `eps` be part of unity hparams ?
    input = ggml_norm(ctx, input, /*eps*/1e-5);
    return ggml_add_inplace(
        ctx,
        ggml_mul_inplace(ctx, ggml_repeat(ctx, weight, input), input),
        ggml_repeat(ctx, bias, input)
    );
}


extern "C" ggml_tensor* StandardFeedForwardNetwork_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs
) {
    seqs = Linear_forward(model, prefix + ".inner_proj", seqs);
    // inner_activation = ReLu // TODO: allow other activation
    seqs = ggml_relu_inplace(model.ctx, seqs);

    if (has_layer(model, prefix + ".inner_layer_norm")) {
        seqs = LayerNorm_forward(model, prefix + ".inner_layer_norm", seqs);
    }

    seqs = Linear_forward(model, prefix + ".output_proj", seqs);
    return seqs;
}


/// Merge the given dimension and the previous one in the tensor.
/// (..., num_heads, N, ...) -> (..., num_heads * N, ...)
/// dim is the position of the resulting merged dimension
/// ggml_flatten_1d(x, d) <==> torch.flatten(x, -1-d-1, -1-d)
ggml_tensor* ggml_flatten_1d(ggml_context* ctx, ggml_tensor* x, int dim) {
    int n_dims = x->n_dims;
    GGML_ASSERT(dim >= 0);
    GGML_ASSERT(dim < n_dims);
    // Nothing to do
    if (dim == n_dims - 1) return x;

    if (n_dims == 2) {
        return ggml_reshape_1d(ctx, x, x->ne[0] * x->ne[1]);
    } else if (n_dims == 3) {
        if (dim == 0) {
            return ggml_reshape_2d(ctx, x, x->ne[0] * x->ne[1], x->ne[2]);
        } else { // dim == 1
            return ggml_reshape_2d(ctx, x, x->ne[0], x->ne[1] * x->ne[2]);
        }
    } else { // n_dims == 4
        if (dim == 0) {
            return ggml_reshape_3d(ctx, x, x->ne[0] * x->ne[1], x->ne[2], x->ne[3]);
        } else if (dim == 1) {
            return ggml_reshape_3d(ctx, x, x->ne[0], x->ne[1] * x->ne[2], x->ne[3]);
        } else { // dim == 2
            return ggml_reshape_3d(ctx, x, x->ne[0], x->ne[1], x->ne[2] * x->ne[3]);
        }
    }
}

/// Split the given dimension.
/// (..., K * N, ...) -> (..., K, N, ...)
/// dim is the position of the output dimension with the given number of element (N).
ggml_tensor* ggml_unflatten_1d(ggml_context* ctx, ggml_tensor* x, int dim, int num_el) {
    int n_dims = x->n_dims;
    GGML_ASSERT(dim >= 0);
    GGML_ASSERT(dim < n_dims);
    GGML_ASSERT(n_dims < 4);
    if (n_dims == 1) {
        return ggml_reshape_2d(ctx, x, num_el, x->ne[0] / num_el);
    } else if (n_dims == 2) {
        if (dim == 0) {
            return ggml_reshape_3d(ctx, x, num_el, x->ne[0] / num_el, x->ne[1]);
        } else { // dim == 1
            return ggml_reshape_3d(ctx, x, num_el, x->ne[0] / num_el, x->ne[1]);
        }
    } else { // (n_dims == 3)
        if (dim == 0) {
            return ggml_reshape_4d(ctx, x, num_el, x->ne[0] / num_el, x->ne[1], x->ne[2]);
        } else if (dim == 1) {
            return ggml_reshape_4d(ctx, x, x->ne[0], num_el, x->ne[1] / num_el, x->ne[2]);
        } else { // dim == 2
            return ggml_reshape_4d(ctx, x, x->ne[0], x->ne[1], num_el, x->ne[2] / num_el);
        }
    }
}


ggml_tensor* _reshape_num_head(ggml_context* ctx, ggml_tensor* x, int head_dim) {
    // (B, S, dim) -> (B, S, H, H_dim)
    x = ggml_unflatten_1d(ctx, x, 0, head_dim);
    // (B?, S, H, H_dim) -> (B?, H, S, H_dim)
    x = ggml_permute(ctx, x, 0, 2, 1, 3);
    return x;
}

/// (B, Sk, dim) -> // (B?, H, H_dim, Sk)
ggml_tensor* _reshape_num_head_values(ggml_context* ctx, ggml_tensor* v, int head_dim ) {
    // (B, Sk, dim) -> (B, Sk, H, H_dim)
    v = ggml_unflatten_1d(ctx, v, 0, head_dim);
    v = ggml_permute(ctx, v, 1, 2, 0, 3);  // (B?, H, H_dim, Sk)
    return v;
}


// flash_attn doesn't work for cross attention because it assumes Q <= K
// TODO: enable flash_attn only for the encoder
# define UNITY_FLASH_ATTN 0

extern "C" ggml_tensor* MultiheadAttention_forward(
    fairseq2_model& model,
    const std::string &prefix,
    ggml_tensor* queries,  // (slen, d_in)
    ggml_tensor* keys,  // (klen, d_in)
    ggml_tensor* values,  // (klen, d_out)
    ggml_tensor* mask // (klen, slen)
) {
    int model_dim = queries->ne[0];
    int num_heads = 16;  // TODO: read from hparams
    int head_dim = model_dim / num_heads;
    GGML_ASSERT(model_dim % num_heads == 0);

    ggml_context* ctx = model.ctx;
    ggml_tensor* q = Linear_forward(model, prefix + ".q_proj", queries);
    q = _reshape_num_head(ctx, q, head_dim);  // (B, H, S, H_dim)
    ggml_set_name(q, "q");
    ggml_tensor* k = Linear_forward(model, prefix + ".k_proj", keys);
    k = _reshape_num_head(ctx, k, head_dim);  // (B, H, Sk, H_dim)
    ggml_set_name(k, "k");

    ggml_tensor* v = Linear_forward(model, prefix + ".v_proj", values);
    v = _reshape_num_head_values(ctx, v, head_dim); // (B, H, H_dim, Sk)
    v = ggml_cont(ctx, v);
    ggml_set_name(v, "v");

#if UNITY_FLASH_ATTN
    // For flash_attn, we assume either no masks, or triangular masks.
    ggml_tensor* attn = ggml_flash_attn(ctx, q, k, v, /*masked*/mask != nullptr);  // (H, S, H_dim)
    ggml_set_name(attn, "attn");
    attn = ggml_permute(ctx, attn, 0, 2, 1, 3); // (B, S, H, H_dim)
    attn = ggml_cont(ctx, attn);
    attn = ggml_flatten_1d(ctx, attn, 0); // (B, S, H * H_dim)
#else
    // (B, H, Sk, H_dim) x (B, H, S, H_dim) -> (B, H, S, Sk)
    ggml_tensor* qk = ggml_mul_mat(ctx, k, q);
    ggml_set_name(qk, "qk");
    ggml_tensor* qk_scale = ggml_new_tensor_1d(ctx, qk->type, 1);
    ggml_set_f32(qk_scale, 1.0f/sqrtf(float(head_dim)));
    qk = ggml_scale(ctx, qk, qk_scale);
    ggml_set_name(qk, "qk_scaled");

    // TODO: Should we replace this by ggml_diag_mask_inf ?
    if (mask) qk = ggml_add(ctx, qk, mask);
    // TODO: upgrade qk to float32 if needed
    ggml_tensor* attn_weights = ggml_soft_max(ctx, qk);  // (B, H, S, Sk)
    ggml_set_name(attn_weights, "attn_weights");

    // (B, H, S, Sk) x (B, H, H_dim, Sk) -> (B, H, H_dim, S)
    ggml_tensor* attn = ggml_mul_mat(ctx, attn_weights, v);
    ggml_set_name(attn, "attn");
    attn = ggml_flatten_1d(ctx, attn, 1); // (B, H * H_dim, S)
    attn = ggml_transpose(ctx, attn); // (B, S, H * H_dim)
    // // I'm not sure why this one is needed ...
    attn = ggml_cont(ctx, attn);
#endif  // UNITY_FLASH_ATTN
    // out -> (B, S, d_out)
    ggml_tensor* out = Linear_forward(model, prefix + ".output_proj", attn);
    ggml_set_name(out, "out");

    return out;
}


extern "C" ggml_tensor* StandardTransformerEncoderLayer_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs,
    ggml_tensor* padding_mask
) {
    ggml_context* ctx = model.ctx;
    // TODO: read norm_order from model
    auto norm_order = TRANSFORMER_NORM_ORDER_PRE;

    // _forward_self_attn(seqs, padding_mask)
    auto residual = seqs;
    if (norm_order != TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".self_attn_layer_norm", seqs);

    // TODO: add padding_mask to MultiheadAttention_forward
    GGML_ASSERT(padding_mask == nullptr);
    seqs = MultiheadAttention_forward(
        model,
        prefix + ".self_attn",
        seqs,
        seqs,
        seqs,
        /*attention masks=*/nullptr
    );

    if (has_layer(model, prefix + ".self_attn_norm"))
        seqs = LayerNorm_forward(model, prefix + ".self_attn_norm", seqs);

    seqs = ggml_add(ctx, seqs, residual);

    if (norm_order == TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".self_attn_layer_norm", seqs);

    // _forward_ffn(seqs)
    residual = seqs;

    if (norm_order != TRANSFORMER_NORM_ORDER_POST)
        seqs = LayerNorm_forward(model, prefix + ".ffn_layer_norm", seqs);

    seqs = StandardFeedForwardNetwork_forward(model, prefix + ".ffn", seqs);

    // TODO: if self.residual_scale is not None:
    // residual = self.residual_scale * residual

    seqs = ggml_add(ctx, seqs, residual);

    if (norm_order == TRANSFORMER_NORM_ORDER_POST)
        seqs = LayerNorm_forward(model, prefix + ".ffn_layer_norm", seqs);

    return seqs;
}

/// ggml_slice(X, -1, start, end) is equivalent to X[start:end]
/// ggml_slice(X, 0, start, end) is equivalent to X[..., start:end]
struct ggml_tensor * ggml_slice(
    struct ggml_context * ctx,
    struct ggml_tensor  * a,
    int axis,
    int64_t start,
    int64_t end
) {
    int64_t ne[4];
    std::copy(a->ne, a->ne + 4, ne);
    if (axis < 0) axis = a->n_dims + axis;
    if (start < 0) start = ne[axis] + start;
    if (end < 0) end = ne[axis] + end;
    GGML_ASSERT(0 <= start);
    GGML_ASSERT(start <= end);
    GGML_ASSERT(end <= ne[axis]);

    ne[axis] = end - start;
    size_t offset = a->nb[axis] * start;

    size_t* nb = a->nb;
    ggml_tensor* result = ggml_view_4d(ctx, a, ne[0], ne[1], ne[2], ne[3], nb[1], nb[2], nb[3], offset);
    result->n_dims = a->n_dims;
    return result;
}


extern "C" ggml_tensor* PositionalEmbedding_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* embeds
) {
    // This only work with the simple pos encoders
    int seq_len = embeds->ne[1];
    ggml_tensor* full_pos_embeds = model.tensors[prefix];
    ggml_tensor* pos_embeds = ggml_slice(model.ctx, full_pos_embeds, /*axis*/1, 0, seq_len);
    return ggml_add(model.ctx, embeds, pos_embeds);
}

extern "C" ggml_tensor* TransformerEmbeddingFrontend_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs
    // TODO: state_bag
) {
    ggml_context* ctx = model.ctx;
    ggml_tensor* embed_weights = model.tensors[prefix + ".embed.weight"];
    GGML_ASSERT(embed_weights != nullptr);
    ggml_tensor* embeds = ggml_get_rows(ctx, embed_weights, seqs);

    // padding mask ?
    // padding_mask = to_padding_mask(embeds, seq_lens)

    if (has_layer(model, prefix + ".pos_encoder")) {
        embeds = PositionalEmbedding_forward(model, prefix + ".pos_encoder", embeds);
    }

    if (has_layer(model, prefix + ".layer_norm")) {
        embeds = LayerNorm_forward(model, prefix + ".layer_norm", embeds);
    }

    return embeds;
}

extern "C" ggml_tensor* StandardTransformerEncoder_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs,
    ggml_tensor* padding_mask
) {
    int layer_idx = 0;
    std::string layer_name = prefix + ".layers." + std::to_string(layer_idx);
    while (has_layer(model, layer_name)) {
        seqs = StandardTransformerEncoderLayer_forward(
            model, layer_name, seqs, padding_mask
        );

        ggml_set_name(seqs, ("x_enc_" + std::to_string(layer_idx)).c_str());
        layer_idx += 1;
        layer_name = prefix + ".layers." + std::to_string(layer_idx);
    }

    if (has_layer(model, prefix + ".layer_norm"))
        seqs = LayerNorm_forward(model, prefix + ".layer_norm", seqs);

    return seqs;
}

extern "C" ggml_tensor* StandardTransformerDecoderLayer_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs,
    ggml_tensor* self_attn_mask,
    ggml_tensor* encoder_output,
    ggml_tensor* encoder_padding_mask
) {
    ggml_context* ctx = model.ctx;
    // TODO: read norm_order from model
    auto norm_order = TRANSFORMER_NORM_ORDER_PRE;

    // _forward_self_attn(seqs, padding_mask)
    auto residual = seqs;
    if (norm_order != TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".self_attn_layer_norm", seqs);

    seqs = MultiheadAttention_forward(
        model,
        prefix + ".self_attn",
        seqs,
        seqs,
        seqs,
        /*attention masks=*/self_attn_mask
    );

    if (has_layer(model, prefix + ".self_attn_norm"))
        seqs = LayerNorm_forward(model, prefix + ".self_attn_norm", seqs);

    seqs = ggml_add(ctx, seqs, residual);

    if (norm_order == TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".self_attn_layer_norm", seqs);

    // _forward_encoder_decoder_attn
    if (! has_layer(model, prefix + ".encoder_decoder_attn")) {
        // `encoder_output` must be `None` for decoder-only attention.
        GGML_ASSERT(encoder_output == nullptr);
        return seqs;
    }

    // `encoder_output` must not be `None` for encoder-decoder attention.
    GGML_ASSERT(encoder_output != nullptr);

    residual = seqs;

    if (norm_order != TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".encoder_decoder_attn_layer_norm", seqs);


    seqs = MultiheadAttention_forward(
        model,
        prefix + ".encoder_decoder_attn",
        seqs,
        encoder_output,
        encoder_output,
        /*attention masks=*/encoder_padding_mask
    );

    seqs = ggml_add(ctx, seqs, residual);

    if (norm_order == TRANSFORMER_NORM_ORDER_POST)
        seqs =  LayerNorm_forward(model, prefix + ".encoder_decoder_attn_layer_norm", seqs);

    // _forward_ffn(seqs)
    residual = seqs;

    if (norm_order != TRANSFORMER_NORM_ORDER_POST)
        seqs = LayerNorm_forward(model, prefix + ".ffn_layer_norm", seqs);

    seqs = StandardFeedForwardNetwork_forward(model, prefix + ".ffn", seqs);

    // TODO:
    // if self.residual_scale is not None:
    // residual = self.residual_scale * residual

    seqs = ggml_add(ctx, seqs, residual);

    if (norm_order == TRANSFORMER_NORM_ORDER_POST)
        seqs = LayerNorm_forward(model, prefix + ".ffn_layer_norm", seqs);

    return seqs;
}

extern "C" ggml_tensor* causal_attention_mask(ggml_context* ctx, ggml_tensor* seqs) {
    auto seq_len = seqs->ne[1];
    // TODO: allow other ggml_type
    ggml_tensor* mask = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, seq_len, seq_len);
    return ggml_diag_mask_inf(ctx, mask, 0);
}

extern "C" ggml_tensor* StandardTransformerDecoder_forward(
    fairseq2_model& model,
    const std::string& prefix,
    ggml_tensor* seqs,
    ggml_tensor* padding_mask,
    ggml_tensor* encoder_output,
    ggml_tensor* encoder_padding_mask
) {
    int layer_idx = 0;
    std::string layer_name = prefix + ".layers." + std::to_string(layer_idx);
    ggml_tensor* self_attn_mask = causal_attention_mask(model.ctx, seqs);
    while (has_layer(model, layer_name)) {
        seqs = StandardTransformerDecoderLayer_forward(
            model, layer_name, seqs, self_attn_mask, encoder_output, encoder_padding_mask
        );

        ggml_set_name(seqs, ("x_dec_" + std::to_string(layer_idx)).c_str());
        layer_idx += 1;
        layer_name = prefix + ".layers." + std::to_string(layer_idx);
    }

    if (has_layer(model, prefix + ".layer_norm"))
        seqs = LayerNorm_forward(model, prefix + ".layer_norm", seqs);

    return seqs;
}

using IncrementalStateBag = std::unordered_map<ggml_tensor*, ggml_tensor*>*;


int _determine_max_seq_len(const SequenceGeneratorJob& job, int source_seq_len) {
    auto opts = job.opts;
    int max_seq_len = -1;
    if (source_seq_len <= 0 || opts.soft_max_seq_len_a <= 0) {
        max_seq_len = opts.hard_max_seq_len;
    } else {
        max_seq_len = std::min(opts.hard_max_seq_len, int(opts.soft_max_seq_len_a * source_seq_len + opts.soft_max_seq_len_b));
    }

    if (opts.min_seq_len > max_seq_len) {
        printf(
            "The effective maximum sequence length must be greater than or equal to `min_seq_len` (%d), but is %d instead. Adjust your soft and hard maximum sequence length limits.\n",
            opts.min_seq_len,
            max_seq_len
        );
        GGML_ASSERT(opts.min_seq_len <= max_seq_len);
    }

    int prefix_seq_len = job.prefix_seq->ne[0];
    if (prefix_seq_len >= max_seq_len) {
        printf(
            "The effective maximum sequence length must be greater than `prefix_seq_len` (%d), but is %d instead.\n",
            prefix_seq_len,
            max_seq_len
        );
        GGML_ASSERT(prefix_seq_len < max_seq_len);
    }

    return max_seq_len;
}

void _fan_out_encoder_output(
    ggml_context* ctx,
    ggml_tensor** encoder_output_out,
    ggml_tensor** encoder_padding_mask_out,
    int beam_size
) {
    // (S_enc, M)
    ggml_tensor* encoder_output = *encoder_output_out;
    ggml_tensor* encoder_padding_mask = *encoder_padding_mask_out;

    // (B, S_enc, M)
    ggml_tensor* shape = ggml_new_tensor_3d(ctx, GGML_TYPE_I8, encoder_output->ne[0], encoder_output->ne[1], beam_size);
    // (S_enc, M) -> (B, S_enc, M)
    *encoder_output_out = ggml_repeat(ctx, encoder_output, shape);
    // (S_enc) -> (B, S_enc)
    ggml_tensor* shape_mask = ggml_new_tensor_2d(ctx, GGML_TYPE_I8, encoder_padding_mask->ne[0], beam_size);
    if (encoder_padding_mask != nullptr) {
        *encoder_padding_mask_out = ggml_repeat(ctx, encoder_padding_mask, shape_mask);
    }
}

ggml_tensor* ggml_log_softmax(ggml_context* ctx, ggml_tensor* logits) {
    // TODO: this isn't the most precise way of doing this
    return ggml_log_inplace(ctx, ggml_soft_max_inplace(ctx, logits));
}

ggml_tensor* ggml_expand_2d(ggml_context* ctx, ggml_tensor* x, int64_t ne0, int64_t ne1) {
    ggml_tensor* shape = ggml_new_tensor_2d(ctx, GGML_TYPE_I8, ne0, ne1);
    ggml_type true_type = x->type;
    x->type = GGML_TYPE_F32;
    ggml_tensor* y = ggml_repeat(ctx, x, shape);
    y->type = true_type;
    return y;
}

void _bootstrap_seqs_and_scores(
    fairseq2_model& model,
    const SequenceGeneratorJob& job,
    ggml_tensor* full_seqs,
    ggml_tensor* scores,
    ggml_tensor* encoder_output,
    ggml_tensor* encoder_padding_mask,
    IncrementalStateBag state_bag
) {
    int prefix_seq_len = job.prefix_seq->ne[0];
    int max_seq_len = scores->ne[0];
    int beam_size = scores->ne[1];
    GGML_ASSERT(prefix_seq_len > 0);
    if (prefix_seq_len == 1)
        return;

    ggml_context* ctx = model.ctx;

    // full_seqs[:, : prefix_seq_len] = job.prefix_seq;
    full_seqs->type = GGML_TYPE_F32;
    job.prefix_seq->type = GGML_TYPE_F32;
    ggml_tensor* seqs = ggml_cpy(ctx, job.prefix_seq, ggml_slice(ctx, full_seqs, 0, 0, prefix_seq_len));

    // We have to bootstrap the model with the already fanned-out encoder
    // output to correctly initialize its incremental state.
    // (S_pfx) -> (N x B, S_pfx - 1)
    // prefix_seq[:-1].expand(beam_size, -1)
    seqs = ggml_expand_2d(ctx, ggml_slice(ctx, seqs, 0, 0, prefix_seq_len - 1), prefix_seq_len - 1, beam_size);
    seqs->type = GGML_TYPE_I32;

    // Bootstrap the model state with prefix sequence.
    seqs = TransformerEmbeddingFrontend_forward(model, "text_decoder_frontend", seqs);
    ggml_tensor* decoder_output = StandardTransformerDecoder_forward(
        model,
        "text_decoder",
        seqs,
        /*padding_mask*/ nullptr,
        encoder_output,
        encoder_padding_mask
        // TODO: state_bag
    );
    // TODO state_bag.increment_step(prefix_seq_len - 1)

    // logits, lprobs: (N, S_pfx - 1, V)
    ggml_tensor* logits = Linear_forward(model, "final_proj", decoder_output);
    int vocab_size = logits->ne[0];
    ggml_tensor* lprobs = ggml_log_softmax(ctx, ggml_slice(ctx, logits, 1, 0, 1));

    ggml_cgraph gf = ggml_build_forward(lprobs);
    ggml_graph_compute_with_ctx(ctx, &gf, 1);
    full_seqs->type = GGML_TYPE_I32;
    job.prefix_seq->type = GGML_TYPE_I32;

    // Fetch scores of next steps from "lprobs"
    float p_score = 0;
    for (int i = 0; i < prefix_seq_len; ++i) {
        int p = ggml_get_i32_1d(job.prefix_seq, i);
        p_score += ggml_get_f32_1d(lprobs, i * vocab_size + p);
        for (int b = 0; b < beam_size; ++b) {
            // scores: (N, S)
            // Note: First step (e.g. BOS)'s score is always 0.
            ggml_set_f32_1d(scores, b * max_seq_len + i + 1, p_score);
        }
    }
}

/// Represents a hypothesis produced by a sequence generator.
struct Hypothesis {
    /// The generated sequence.
    ggml_tensor* seq;

    /// The score of the hypothesis.
    float score;

    /// The score of each individual sequence step.
    ggml_tensor* step_scores;
};


/// Represents a standard beam search algoritm.
int StandardBeamSearch_step(
    ggml_context* ctx,
    int step_nr,
    bool is_start_step,
    ggml_tensor* lprobs,  // (B, V)
    ggml_tensor* last_scores,  // (B)
    ggml_tensor* candidate_indices
) {
    GGML_ASSERT(lprobs->n_dims == 2);
    int vocab_size = lprobs->ne[0];
    int beam_size = lprobs->ne[1];
    GGML_ASSERT(last_scores->n_dims == 2);
    GGML_ASSERT(last_scores->ne[0] == 1);
    GGML_ASSERT(last_scores->ne[1] == beam_size);
    GGML_ASSERT(candidate_indices->ne[0] == beam_size * vocab_size);

    // should this be done by the caller ?
    if (is_start_step) {
        // At the initial step, all hypotheses are equally likely, so we use
        // only the first beam.
        lprobs = ggml_slice(ctx, lprobs, 1, 0, 1);
        lprobs = ggml_cont(ctx, lprobs);
        // The first step always indicates the beginning of the sequence and
        // has no score.
        if (step_nr > 0) {
            lprobs = ggml_add_inplace(ctx, lprobs, ggml_repeat(ctx, last_scores, lprobs));
        }
    } else {
        // Make probabilities contain cumulative scores for each hypothesis.
        // TODO this seems incorrect
        lprobs = ggml_add(ctx, lprobs, ggml_repeat(ctx, last_scores, lprobs));
    }

    ggml_cgraph gf = ggml_build_forward(lprobs);
    ggml_graph_compute_with_ctx(ctx, &gf, 1);

    // Take the best 2 x `beam_size` predictions. We'll choose the first
    // `beam_size` of these which don't predict EOS to continue with.
    // (N, 2 x B)
    // `vocab_size` - 1 to never select PAD.
    int topk = std::min(2 * beam_size, vocab_size - 1);

    auto comp = [lprobs](std::int32_t a, std::int32_t b) {
        return ggml_get_f32_1d(lprobs, a) > ggml_get_f32_1d(lprobs, b);
    };
    auto cand = (std::int32_t*)candidate_indices->data;
    std::partial_sort(cand, cand + topk, cand + (beam_size * vocab_size), comp);

    return topk;
}


void ggml_detach(ggml_tensor* a) {
    a->op = GGML_OP_NONE;
    a->src[0] = nullptr;
}


int _finalize_hypothesis(
    const SequenceGeneratorJob& job,
    ggml_context* ctx,
    int step_nr,
    int vocab_size,
    std::int32_t candidate,
    float tok_score,
    ggml_tensor* seqs, // (beam_size, seq_len)
    ggml_tensor* scores, // (beam_size, seq_len)
    std::vector<Hypothesis>& hypotheses
) {
    std::int32_t beam = candidate / vocab_size;
    std::int32_t token = candidate % vocab_size;

    // Detect beams that reached the minimum length and that end with an EOS.
    bool eos = token == job.eos_idx;
    eos &= tok_score != -INFINITY;

    if (!eos) return 0;

    // If the candidate beam is "finished", let's copy the score and sequence
    ggml_tensor* tokens = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, step_nr + 2);
    ggml_tensor* step_scores = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, step_nr + 2);

    auto tok = (std::int32_t*)tokens->data;
    for (int i = 0; i < step_nr + 1; ++i) {
        tok[i] = ggml_get_i32_1d(seqs, seqs->ne[0] * beam + i);
    }
    tok[step_nr + 1] = token;

    // Convert from cumulative to per-step scores.
    auto sc = (float*)step_scores->data;
    float last_score = tok_score;
    for (int i = step_nr; i >= 0; --i) {
        float sc0 = ggml_get_f32_1d(scores, scores->ne[0] * beam + i);
        sc[i] = last_score - sc0;
        last_score = sc0;
    }

    if (job.opts.normalize_scores)
        // Skip first EOS since it is always 0 and skews normalization.
        tok_score /= (float)std::pow((step_nr + 1), job.opts.len_penalty);

    // TODO the score computed here isn't the same than computed by fairseq2.
    hypotheses.emplace_back(Hypothesis{tokens, tok_score, step_scores});
    return 1;
}

/// Generates a translation for a single sequence
// TODO: finish this for beam_size=1
// * find out why score is different (seq is the same though)
// TODO: add IncrementalStateBag support to avoid a O(N^3) generation.
// TODO: support beam_size > 1:
// * most layers assume un-batched input, but we want to handle several beams at once
// * need to port "reorder_state_dict"
// TODO: clean up
// * replace manual tensor tweaking with ggml_set_*d (ggml_set_slice could be useful)
extern "C" float generate_sequence(
    fairseq2_model& model,
    const SequenceGeneratorJob& job,
    ggml_tensor* encoder_output,
    ggml_tensor* encoder_padding_mask,
    ggml_tensor* output_seq
) {
    ggml_context* ctx = model.ctx;
    size_t eos_idx = job.eos_idx;
    auto pad_idx = job.pad_idx;

    ggml_tensor* embed = model.tensors["text_decoder_frontend.embed.weight"];
    size_t vocab_size = embed->ne[1];
    std::size_t beam_size = job.opts.beam_size;
    int source_seq_len = encoder_output->ne[1];
    int max_seq_len = _determine_max_seq_len(job, source_seq_len);

    // (S_enc, M) -> (B, S_enc, M)
    _fan_out_encoder_output(ctx, &encoder_output, &encoder_padding_mask, beam_size);

    std::vector<Hypothesis> finished_searches;
    finished_searches.reserve(beam_size);

    // Initialize buffers. (B, S)
    ggml_tensor* seqs = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, max_seq_len, beam_size);
    ggml_set_i32(seqs, 0);
    ggml_tensor* scores = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, max_seq_len, beam_size);
    ggml_set_f32(scores, 0.0);

    IncrementalStateBag state_bag = {};
    _bootstrap_seqs_and_scores(
        model, job, seqs, scores, encoder_output, encoder_padding_mask, state_bag
    );
    int prefix_seq_len = job.prefix_seq->ne[0];
    int start_step = prefix_seq_len - 1;

    // Holds the indices of beams (a beam can occur more than once) that we
    // should continue with in the next step.
    ggml_tensor* beam_indices = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, beam_size);
    ggml_tensor* next_tokens = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, beam_size);
    ggml_tensor* next_scores = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, beam_size);

    // Array with integers up to 'vocab_size * beam_size' to represent next beams to explore
    ggml_tensor* candidate_indices = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, vocab_size * beam_size);
    for (std::size_t i = 0; i < vocab_size * beam_size; ++i)
        ((int32_t *)(candidate_indices->data))[i] = i;

    // TODO: memory management
    // there should be a per-step ggml_context for intermediary results
    // start of beam search:
    for (int step_nr = start_step; step_nr < max_seq_len - 1; ++step_nr) {
        // if (beam_indices != nullptr) {
        //     // If not `None`, it means in the last step we finalized one or
        //     // more searches. We should ensure that we adjust `beam_indices`
        //     // before reordering `decoder`'s incremental state.
        //     if (search_indices != nullptr) {
        //         num_searches = search_indices->ne[0];

        //         // (N)
        //         delta = search_indices - torch.arange(num_searches, device=device)

        //         // (N) -> (N, 1)
        //         delta.unsqueeze_(-1)

        //         // Adjust indices to take into account removed searches.
        //         beam_indices.view(num_searches, beam_size).add_(delta * beam_size)
        //     }

        //     // state_bag.reorder(beam_indices)
        // }
        // because of no IncrementalStateBag we pass input from the start
        // decoder_input = seqs[:, 0 : step_nr + 1]
        ggml_tensor* decoder_input = ggml_slice(ctx, seqs, 0, 0, step_nr + 1);
        decoder_input = TransformerEmbeddingFrontend_forward(model, "text_decoder_frontend", decoder_input);
        ggml_tensor* decoder_output = StandardTransformerDecoder_forward(
            model,
            "text_decoder",
            decoder_input,
            nullptr,  // We never generate PAD.
            encoder_output,
            encoder_padding_mask
            // state_bag=state_bag,
        );

        // state_bag.increment_step()

        // Because of no IncrementalStateBag decoder_output here is of shape (B, S, D)
        // Just look at the last token.
        decoder_output = ggml_slice(ctx, decoder_output, 1, step_nr, step_nr+1);
        ggml_tensor* logits = Linear_forward(model, "final_proj", decoder_output);
        ggml_tensor* lprobs = ggml_log_softmax(ctx, logits);

        // Compute lprobs here so we can modify it in place in the lprob tweaking phase
        // TODO: use ggml properly compute the tweaks
        ggml_cgraph gf = ggml_build_forward(lprobs);
        printf("beam search step %d. Graph.n_nodes: %d\n", step_nr, gf.n_nodes);
        ggml_graph_compute_with_ctx(ctx, &gf, 1);
        ggml_detach(lprobs);

        // // Do not allow EOS before reaching the minimum sequence length.
        if (step_nr < job.opts.min_seq_len) {
            // lprobs[:, :, self.eos_idx] = -INFINITY;
            for (size_t i = 0; i < beam_size; ++i)
                ggml_set_f32_1d(lprobs, vocab_size * i + eos_idx, -INFINITY);
        }

        // If we have reached the maximum length, force the last step to be EOS.
        // TODO: should this be done in an adhoc loop ? how often does that happen anyway ?
        if (step_nr == max_seq_len - 2) {
            // lprobs[:, :, : self.eos_idx]       = -torch.inf
            // lprobs[:, :,   self.eos_idx + 1 :] = -torch.inf
            for (size_t b = 0; b < beam_size; ++b) {
                size_t t = 0;
                for (t = 0; t < eos_idx; ++t)
                    ggml_set_f32_1d(lprobs, vocab_size * b + t, -INFINITY);
                for (t = eos_idx + 1; t < vocab_size; ++t)
                    ggml_set_f32_1d(lprobs, vocab_size * b + t, -INFINITY);
            }

        }

        // Never allow PAD.
        for (size_t i = 0; i < beam_size; ++i)
            ggml_set_f32_1d(lprobs, vocab_size * i + pad_idx, -INFINITY);

        // Apply UNK penalty.
        if (job.unk_idx >= 0 && job.opts.unk_penalty != 0) {
            // lprobs[:, :, self.unk_idx] -= self.opts.unk_penalty
            auto lprobs_raw = ggml_get_data_f32(lprobs);
            for (size_t i = 0; i < beam_size; ++i)
                lprobs_raw[vocab_size * i + job.unk_idx] -= job.opts.unk_penalty;
        }


        // Determine candidates for the next step.
        // (N, 2 x B)
        int topk = StandardBeamSearch_step(
            ctx,
            step_nr,
            step_nr == start_step,
            lprobs,
            ggml_slice(ctx, scores, 0, step_nr, step_nr+1),
            candidate_indices
        );

        std::size_t ongoing_beams = 0;
        int new_num_searches = 0;
        for (std::int32_t i = 0; i < topk; ++i) {
            int c = ggml_get_f32_1d(candidate_indices, i);
            float tok_score = ggml_get_f32_1d(lprobs, c);
            int finished = _finalize_hypothesis(job, ctx, step_nr, vocab_size, c, tok_score, seqs, scores, finished_searches);
            new_num_searches += finished;
            if (!finished){
                std::int32_t beam = c / vocab_size;
                std::int32_t token = c % vocab_size;

                ggml_set_f32_1d(beam_indices, ongoing_beams, beam);
                ggml_set_f32_1d(next_tokens, ongoing_beams, token);
                ggml_set_f32_1d(next_scores, ongoing_beams, tok_score);
                ongoing_beams += 1 - finished;
            }
            if (ongoing_beams >= beam_size) break;
            if (finished_searches.size() >= beam_size)
                goto end_of_beam_search;
        }

        // Reorder beams in the `seq` and `score` buffers. The same beam can
        // be selected more than once.
        ggml_tensor* new_seqs = seqs;
        // ggml_get_rows and ggml_set only work with floats ...
        new_seqs->type = GGML_TYPE_F32;
        ggml_tensor* new_scores = scores;
        if (step_nr > start_step) {
            // (B, S), (B) -> (B, S)
            new_seqs = ggml_get_rows(ctx, seqs, beam_indices);
            new_scores = ggml_get_rows(ctx, new_scores, beam_indices);
        }

        // new_seqs[:, step_nr + 1] = next_tokens
        gf = ggml_build_forward(ggml_set_1d_inplace(ctx, new_seqs, next_tokens, new_seqs->nb[0] * (step_nr + 1)));
        ggml_graph_compute_with_ctx(ctx, &gf, 1);
        ggml_detach(new_seqs);
        new_seqs->type = GGML_TYPE_I32;

        gf = ggml_build_forward(ggml_set_1d_inplace(ctx, new_scores, next_scores, new_scores->nb[0] * (step_nr + 1)));
        ggml_graph_compute_with_ctx(ctx, &gf, 1);
        ggml_detach(new_scores);

        // TODO the old seqs and score buffers could be reused for next step
        seqs = new_seqs;
        scores = new_scores;
    }

end_of_beam_search:
    // Ensure that hypotheses are sorted by decreasing scores before returning.
    std::sort(
        finished_searches.begin(),
        finished_searches.end(),
        [](Hypothesis a, Hypothesis b) { return a.score > b.score; }
    );

    // For now just return the best sequence
    // TODO: return structured output
    *output_seq = *(finished_searches[0].seq);

    return finished_searches[0].score;
}