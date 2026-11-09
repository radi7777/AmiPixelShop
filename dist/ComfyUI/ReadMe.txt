AmiPixelShop - ComfyUI workflows
================================

With "Provider: ComfyUI (LAN)" in Edit > AI Settings, the AI functions run
on a PC in your network with ComfyUI. Free and private: nothing leaves your LAN.

On the PC
  - Start ComfyUI with  --listen  so the Amiga can connect
    (e.g.  python main.py --listen ). Port 8188 must be open in the firewall.
  - Install the models used by the workflow.

On the Amiga
  - Edit > AI Settings...: Provider ComfyUI (LAN)
  - Edit > AI API Key...: address of the PC, e.g. 192.168.1.20:8188

Workflows (ComfyUI "API format"; AmiPixelShop looks in ENV:AmiPixelShop/,
ENVARC:AmiPixelShop/ and then in this drawer)

  comfy_fill.json      Generative Fill. Also Remove and Extend, unless
                       comfy_remove.json exists. Included: Flux.1 Fill dev
                       (flux1-fill-dev, clip_l, t5xxl_fp16, ae; ~16 GB VRAM,
                       or use an fp8/GGUF variant).
  comfy_fill_gguf.json The same with a GGUF model, e.g. flux1-fill-dev-Q6_K.gguf
                       (much smaller). Needs the ComfyUI-GGUF custom node
                       (install it with ComfyUI-Manager, then restart
                       ComfyUI) - the standard model loader does not list
                       .gguf files. Put the .gguf into models/unet or
                       models/diffusion_models, adjust unet_name, and save
                       it as comfy_fill.json. The T5 text encoder may also
                       be a .gguf file.
  comfy_fill_flux3.json  FLUX 3 Image Klein (int8, runs on 8 GB GPUs), an
                       editing model: it gets the whole crop plus an
                       instruction with the region (%task%), and its prompt
                       upsampler (Qwen3.5-9B) turns that into the caption
                       the model needs. Files from huggingface.co/
                       black-forest-labs/flux-3-image-klein-comfy (accept the
                       license there first): flux3-image-klein-int8-convrot
                       (diffusion_models), qwen3vl_4b_int8_convrot and
                       qwen3.5_9b_flux3_prompt_upsampler_int8_convrot
                       (text_encoders), flux2-vae (vae). Needs a current
                       ComfyUI. Save it as comfy_fill.json. License of the
                       model: FLUX Kommunity License (non-commercial).
  comfy_fill_flux2klein.json  FLUX.2 Klein 4B (distilled, 4 steps): the same
                       kind of editing model, smaller and faster, without a
                       prompt upsampler (gets %task% directly). Apache 2.0,
                       no login needed: huggingface.co/Comfy-Org/
                       Flux-2-Klein-4B - flux-2-klein-4b (diffusion_models),
                       qwen_3_4b (text_encoders), flux2-vae (vae). Save it
                       as comfy_fill.json - or as comfy_remove.json, to use
                       it for Remove/Extend next to another Fill workflow.
  comfy_fill_sd.json   Alternative for smaller GPUs: any SD 1.5 / SDXL
                       inpainting checkpoint. Put your model name into
                       ckpt_name and save it as comfy_fill.json.
  comfy_remove.json    Optional, for Remove and Extend (e.g. with LaMa).
  comfy_segment.json   Optional, for Select Subject and Remove Background.
                       Needs a background-removal node pack (BiRefNet, RMBG,
                       ...): Load Image -> remover (MASK) -> Convert Mask to
                       Image -> Save Image. White = subject.

Your own workflow
  1. Build it in ComfyUI: Load Image -> ... -> Save Image. Load Image gives
     the picture and, from its transparency, the MASK of the area to fill.
  2. Workflow > Export (API).
  3. In a text editor: "image": "%image%" in the Load Image node,
     "%prompt%" in the prompt text, and optionally "seed": "%seed%"
     (a new seed for every request, so Generate again gives new variants).
     Placeholders:
       %prompt%  the text you typed (for Remove/Extend a fitting default) -
                 for inpainting models, which only paint inside the mask
       %task%    a complete English instruction including the region, for
                 editing models that see the whole image (FLUX 3, Kontext,
                 Qwen-Image-Edit ...)
       %box%     just the region: [y_min, x_min, y_max, x_max], 0 to 1
       %image%, %seed%  as above
     Note: if you open one of these workflows in ComfyUI's editor, "%seed%"
     is not a valid number there, so the seed field shows empty or 0 -
     enter any number (e.g. 12345) to try it in ComfyUI, but put "%seed%"
     back before saving it for AmiPixelShop. With a fixed number every
     request gives the same picture.
  4. Save it as ENVARC:AmiPixelShop/comfy_fill.json (or remove/segment).

If a model is missing on the PC, the error message lists the models ComfyUI has.
