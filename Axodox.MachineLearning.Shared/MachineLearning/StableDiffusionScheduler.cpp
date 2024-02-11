#include "pch.h"
#include "StableDiffusionScheduler.h"

using namespace std;

namespace Axodox::MachineLearning
{
  const size_t StableDiffusionSchedulerSteps::DerivativeOrder = 4;

  StableDiffusionScheduler::StableDiffusionScheduler(const StableDiffusionSchedulerOptions& options) :
    _options(options)
  {
    auto betas = _options.BetasTrained;

    if (betas.empty())
    {
      switch (_options.BetaSchedulerType)
      {
      case StableDiffusionBetaSchedulerKind::Linear:
        betas = GetLinearBetas();
        break;
      case StableDiffusionBetaSchedulerKind::ScaledLinear:
        betas = GetScaledLinearBetas();
        break;
      default:
        throw logic_error("StableDiffusionBetaSchedulerKind not implemented.");
      }
    }
    else
    {
      if (betas.size() != _options.TrainStepCount) throw invalid_argument("options.BetasTrained.Size() != options.TrainStepCount");
    }

    _cumulativeAlphas = CalculateCumulativeAlphas(betas);
    _initialNoiseSigma = CalculateInitialNoiseSigma(_cumulativeAlphas);
  }

  float IntegrateOverInterval(const std::function<float(float)>& f, float intervalStart, float intervalEnd)
  {
    auto stepCount = 100;
    auto stepSize = (intervalEnd - intervalStart) / stepCount;

    auto result = 0.f;
    for (auto value = intervalStart; value < intervalEnd; value += stepSize)
    {
      result += f(value) * stepSize;
    }
    return result;
  }

  StableDiffusionSchedulerSteps StableDiffusionScheduler::GetSteps(size_t count) const
  {
    //Calculate timesteps
    vector<float> timesteps;
    timesteps.resize(count);

    auto step = (_options.TrainStepCount - 1) / float(count - 1);
    for (auto value = 0.f; auto & timestep : timesteps)
    {
      timestep = value;
      value += step;
    }

    //Calculate sigmas
    vector<float> sigmas{ _cumulativeAlphas.rbegin(), _cumulativeAlphas.rend() };
    for (auto& sigma : sigmas)
    {
      sigma = sqrt((1.f - sigma) / sigma);
    }

    vector<float> interpolatedSigmas;
    interpolatedSigmas.reserve(count);
    interpolatedSigmas.resize(count);
    for (size_t i = 0; auto & interpolatedSigma : interpolatedSigmas)
    {
      auto trainStep = timesteps[i++];
      auto previousIndex = max(size_t(floor(trainStep)), size_t(0));
      auto nextIndex = min(size_t(ceil(trainStep)), sigmas.size() - 1);
      interpolatedSigma = lerp(sigmas[previousIndex], sigmas[nextIndex], trainStep - floor(trainStep));
    }
    interpolatedSigmas.push_back(0.f);

    if (_options.SchedulerType == StableDiffusionSchedulerKind::DpmPlusPlus2M)
    {
      ApplyKarrasSigmas(interpolatedSigmas);
    }
    ranges::reverse(timesteps);

    //Calculate LMS coefficients
    coefficients_t coefficients;
    for (size_t i = 0; i < count; i++)
    {
      switch (_options.SchedulerType)
      {
      case StableDiffusionSchedulerKind::LmsDiscrete:
        coefficients.push_back(GetLmsCoefficients(i, interpolatedSigmas));
        break;
      case StableDiffusionSchedulerKind::EulerAncestral:
        coefficients.push_back(GetEulerCoefficients(i, interpolatedSigmas));
        break;
      case StableDiffusionSchedulerKind::DpmPlusPlus2M:
        break;
      default:
        throw logic_error("Scheduler not implemented.");
      }
    }
    
    //Return result
    StableDiffusionSchedulerSteps result;
    result.Timesteps = move(timesteps);
    result.Sigmas = move(interpolatedSigmas);
    result.Coefficients = move(coefficients);
    result.SchedulerType = _options.SchedulerType;
    return result;
  }

  float StableDiffusionScheduler::InitialNoiseSigma() const
  {
    return _initialNoiseSigma;
  }

  std::span<const float> StableDiffusionScheduler::CumulativeAlphas() const
  {
    return _cumulativeAlphas;
  }

  std::vector<float> StableDiffusionScheduler::GetLinearBetas() const
  {
    vector<float> results;
    results.resize(_options.TrainStepCount);

    auto value = _options.BetaAtStart;
    auto step = (_options.BetaAtEnd - _options.BetaAtStart) / (_options.TrainStepCount - 1.f);
    for (auto& beta : results)
    {
      beta = value;
      value += step;
    }

    return results;
  }

  std::vector<float> StableDiffusionScheduler::GetScaledLinearBetas() const
  {
    vector<float> results;
    results.resize(_options.TrainStepCount);

    auto value = sqrt(_options.BetaAtStart);
    auto step = (sqrt(_options.BetaAtEnd) - value) / (_options.TrainStepCount - 1.f);
    for (auto& beta : results)
    {
      beta = value * value;
      value += step;
    }

    return results;
  }
  
  std::vector<float> StableDiffusionScheduler::CalculateCumulativeAlphas(std::span<const float> betas)
  {
    vector<float> results{ betas.begin(), betas.end() };

    float value = 1.f;
    for (auto& result : results)
    {
      value *= 1.f - result;
      result = value;
    }

    return results;
  }
  
  float StableDiffusionScheduler::CalculateInitialNoiseSigma(std::span<const float> cumulativeAlphas)
  {
    float result = 0;

    for (auto cumulativeAlpha : cumulativeAlphas)
    {
      auto sigma = sqrt((1.f - cumulativeAlpha) / cumulativeAlpha);
      if (sigma > result) result = sigma;
    }

    return result;
  }

  LmsCoefficients StableDiffusionScheduler::GetLmsCoefficients(size_t i, std::span<const float> sigmas)
  {
    auto order = min(i + 1, StableDiffusionSchedulerSteps::DerivativeOrder);

    LmsCoefficients results;
    for (auto j = 0; j < order; j++)
    {
      auto lmsDerivative = [&](float tau) {
        float product = 1.f;
        for (size_t k = 0; k < order; k++)
        {
          if (j == k)
          {
            continue;
          }
          product *= (tau - sigmas[i - k]) / (sigmas[i - j] - sigmas[i - k]);
        }
        return product;
      };

      results.push_back(-IntegrateOverInterval(lmsDerivative, sigmas[i + 1], sigmas[i]));
    }

    return results;
  }

  EulerCoefficients StableDiffusionScheduler::GetEulerCoefficients(size_t step, std::span<const float> sigmas)
  {
    auto sigmaFrom = sigmas[step];
    auto sigmaTo = sigmas[step + 1];

    auto sigmaFromLessSigmaTo = sigmaFrom * sigmaFrom - sigmaTo * sigmaTo;
    auto sigmaUpResult = (sigmaTo * sigmaTo * sigmaFromLessSigmaTo) / (sigmaFrom * sigmaFrom);
    auto sigmaUp = sigmaUpResult < 0 ? -sqrt(abs(sigmaUpResult)) : sqrt(sigmaUpResult);

    auto sigmaDownResult = sigmaTo * sigmaTo - sigmaUp * sigmaUp;
    auto sigmaDown = sigmaDownResult < 0 ? -sqrt(abs(sigmaDownResult)) : sqrt(sigmaDownResult);

    return EulerCoefficients{
      .SigmaDown = sigmaDown,
      .SigmaUp = sigmaUp
    };
  }

  void StableDiffusionScheduler::ApplyKarrasSigmas(std::span<float> sigmas)
  {
    const auto rho = 7.f;

    auto sigmaMax = sigmas.front();
    auto sigmaMin = *(sigmas.end() - 2);

    auto invRhoMin = pow(sigmaMin, 1.f / rho);
    auto invRhoMax = pow(sigmaMax, 1.f / rho);

    auto stepCount = sigmas.size() - 1;
    auto stepSize = 1.f / (stepCount - 1);
    for (auto i = 0; i < stepCount; i++)
    {
      auto t = i * stepSize;
      sigmas[i] = pow(invRhoMax + t * (invRhoMin - invRhoMax), rho);
    }
  }
  
  Tensor StableDiffusionSchedulerSteps::ApplyStep(const Tensor& sample, const Tensor& output, list<Tensor>& derivatives, std::span<std::minstd_rand> randoms, size_t step)
  {
    auto sigma = Sigmas[step];

    //Compute predicted original sample (x_0) from sigma-scaled predicted noise
    auto predictedOriginalSample = sample.BinaryOperation<float>(output, [sigma](float a, float b) { return a - sigma * b; });

    //Calculate new latents
    switch (SchedulerType)
    {
    case StableDiffusionSchedulerKind::LmsDiscrete:
    {
      auto currentDerivative = sample.BinaryOperation<float>(predictedOriginalSample, [sigma](float a, float b) { return (a - b) / sigma; });

      derivatives.push_back(currentDerivative);
      if (derivatives.size() > DerivativeOrder) derivatives.pop_front();

      auto& derivativeCoefficients = get<LmsCoefficients>(Coefficients[step]);

      vector<Tensor> lmsDerivativeProduct;
      lmsDerivativeProduct.reserve(derivatives.size());
      for (auto i = 0; auto & derivative : ranges::reverse_view(derivatives))
      {
        lmsDerivativeProduct.push_back(derivative * derivativeCoefficients[i++]);
      }

      Tensor latentDelta{ TensorType::Single, currentDerivative.Shape };
      for (auto& tensor : lmsDerivativeProduct)
      {
        latentDelta.UnaryOperation<float>(tensor, [](float a, float b) { return a + b; });
      }

      return sample.BinaryOperation<float>(latentDelta, [](float a, float b) { return a + b; });
    }
    case StableDiffusionSchedulerKind::EulerAncestral:
    {
      auto currentDerivative = sample.BinaryOperation<float>(predictedOriginalSample, [sigma](float a, float b) { return (a - b) / sigma; });

      auto& eulerCoefficient = get<EulerCoefficients>(Coefficients[step]);

      auto dt = eulerCoefficient.SigmaDown - sigma;
      auto randomNoise = Tensor::CreateRandom(sample.Shape, randoms, eulerCoefficient.SigmaUp);
      auto latentDelta = randomNoise.BinaryOperation<float>(currentDerivative, [dt](float a, float b) { return a + dt * b; });

      return sample.BinaryOperation<float>(latentDelta, [](float a, float b) { return a + b; });
    }
    case StableDiffusionSchedulerKind::DpmPlusPlus2M:
    {
      //K-diffusion nomenclature
      //x => sample
      //denoised => predictedOriginalSample
      //d => currentDerivative

      auto t = -log(sigma);
      auto tNext = -log(Sigmas[step + 1]);
      auto h = tNext - t;

      derivatives.push_back(Tensor(predictedOriginalSample));

      if (derivatives.size() > 1 && Sigmas[step + 1] != 0)
      {
        auto hLast = t + log(Sigmas[step - 1]);
        auto r = hLast / h;

        OutputDebugString(std::format(L"i: {}, h_last: {}, r : {}, x : {}, y : {}\n", step, hLast, r, exp(-tNext) / exp(-t), exp(-h) - 1.f).c_str());

        predictedOriginalSample.UnaryOperation<float>(derivatives.front(), [
          x = 1.f + 1.f / (2.f * r),
          y = 1.f / (2.f * r)](float a, float b) 
          { return x * a - y * b; });

        derivatives.pop_front();
      }
      else
      {
        OutputDebugString(std::format(L"i: {}, x : {}, y : {}\n", step, exp(-tNext) / exp(-t), exp(-h) - 1.f).c_str());
      }

      return sample.BinaryOperation<float>(predictedOriginalSample, [
        x = exp(-tNext) / exp(-t),
        y = exp(-h) - 1.f](float a, float b) 
        { return x * a - y * b; });
    }
    default:
      throw logic_error("Scheduler not implemented.");
    }
  }
}