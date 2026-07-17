import { ComponentFixture, TestBed } from '@angular/core/testing';

import { EditComponent } from './edit.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { provideRouter } from '@angular/router';
import { FormBuilder } from '@angular/forms';
import { SystemAsic as ISystemASIC } from 'src/app/generated/models';

describe('EditComponent', () => {
  let component: EditComponent;
  let fixture: ComponentFixture<EditComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      imports: [EditComponent],
      providers: [provideHttpClient(), provideToastr(), provideRouter([])]
    });
    fixture = TestBed.createComponent(EditComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('disables fixed BZM frequency and voltage settings', () => {
    const fb = TestBed.inject(FormBuilder);
    component.form = fb.group({
      frequency: [50],
      coreVoltage: [2800],
    });

    component.applyAsicCapabilities({
      ASICModel: 'BZM',
      deviceModel: 'Bonanza',
      swarmColor: 'yellow',
      asicCount: 4,
      defaultFrequency: 50,
      frequencyOptions: [50],
      frequencyTunable: false,
      defaultVoltage: 2800,
      voltageOptions: [2800],
      voltageTunable: false,
    } as ISystemASIC);

    expect(component.hasTunableAsicSettings).toBeFalse();
    expect(component.form.controls['frequency'].disabled).toBeTrue();
    expect(component.form.controls['coreVoltage'].disabled).toBeTrue();
  });
});
